// Truck Monitor Phase 3: sensor node with WiFi sync for the Heltec WiFi LoRa32 V3.
// Each wake: read sensors, append the record to flash, try to upload everything
// queued, then deep sleep. A record leaves flash only after CT100 confirms it.
// Sensors sit on GPIO41/42, separate from the OLED bus on GPIO17/18.
//
// Network settings live in NVS, set over USB within CFG_WINDOW_MS of a reset:
//   cfg<TAB>ssid<TAB>psk<TAB>host<TAB>port<TAB>token
// See tools/set_net.py. They are never printed.

#include <Wire.h>
#include <esp_sleep.h>
#include <esp_random.h>
#include <time.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <Adafruit_INA219.h>
#include <Adafruit_AHTX0.h>
#include <Adafruit_BMP280.h>
#include <Adafruit_SSD1306.h>
#include <driver/gpio.h>

// Bench diagnostics screen. Set to 0 for the truck to save battery.
#define ENABLE_OLED 1

static const char *DEVICE_ID = "truck1";
static const int PIN_SDA = 41;
static const int PIN_SCL = 42;
static const uint8_t BMP280_ADDR = 0x76;
static const uint64_t SLEEP_SECONDS = 20;

static const char *QUEUE_PATH = "/queue.jsonl";
static const char *QUEUE_TMP = "/queue.tmp";
static const size_t BATCH_BYTES = 6000;
static const uint32_t WIFI_TIMEOUT_MS = 6000;
static const uint32_t NTP_TIMEOUT_MS = 3000;
static const uint32_t HTTP_TIMEOUT_MS = 4000;
static const uint32_t UPLOAD_BUDGET_MS = 15000;
static const uint32_t CFG_WINDOW_MS = 4000;
static const time_t MIN_VALID_EPOCH = 1600000000;

static const int PIN_OLED_SDA = 17;
static const int PIN_OLED_SCL = 18;
static const int PIN_OLED_RST = 21;
static const gpio_num_t PIN_VEXT = GPIO_NUM_36;  // Low turns the OLED rail on.
static const uint8_t OLED_ADDR = 0x3C;
static const uint32_t OLED_HOLD_MS = 3000;

// Survive deep sleep. bootId changes on power-up, so (bootId, seq) stays unique
// even when seq restarts.
RTC_DATA_ATTR uint32_t bootId = 0;
RTC_DATA_ATTR uint32_t seq = 0;

Adafruit_INA219 ina219;
Adafruit_AHTX0 aht;
Adafruit_BMP280 bmp(&Wire);

struct NetConfig {
  String ssid, psk, host, token;
  int port = 0;
  bool valid() const { return ssid.length() && host.length() && token.length() && port > 0; }
};

struct SyncResult {
  bool wifi = false;
  int sent = 0;
  int httpCode = 0;
  uint32_t ms = 0;
};

static NetConfig loadNet() {
  NetConfig n;
  Preferences p;
  if (p.begin("net", true)) {
    n.ssid = p.getString("ssid", "");
    n.psk = p.getString("psk", "");
    n.host = p.getString("host", "");
    n.token = p.getString("token", "");
    n.port = p.getInt("port", 0);
    p.end();
  }
  return n;
}

static bool saveNet(const String &line) {
  String part[5];
  int start = line.indexOf('\t') + 1;
  for (int i = 0; i < 5; i++) {
    int tab = line.indexOf('\t', start);
    if (tab < 0 && i < 4) {
      return false;
    }
    part[i] = tab < 0 ? line.substring(start) : line.substring(start, tab);
    start = tab + 1;
  }
  Preferences p;
  if (!p.begin("net", false)) {
    return false;
  }
  p.putString("ssid", part[0]);
  p.putString("psk", part[1]);
  p.putString("host", part[2]);
  p.putInt("port", part[3].toInt());
  p.putString("token", part[4]);
  p.end();
  return true;
}

// Listens briefly after a reset so settings can change without reflashing.
static void configWindow() {
  Serial.println("# cfg window");
  Serial.setTimeout(CFG_WINDOW_MS);
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.startsWith("cfg\t")) {
    Serial.println(saveNet(line) ? "# cfg saved" : "# cfg rejected");
  }
}

static void appendField(String &out, const char *name, float value, int decimals) {
  out += '"';
  out += name;
  out += "\":";
  if (isnan(value)) {
    out += "null";
  } else {
    out += String(value, decimals);
  }
  out += ',';
}

static bool enqueue(const String &record) {
  File f = LittleFS.open(QUEUE_PATH, FILE_APPEND);
  if (!f) {
    return false;
  }
  bool ok = f.println(record) == record.length() + 2;
  f.close();
  return ok;
}

static int queuedRecords() {
  File f = LittleFS.open(QUEUE_PATH, FILE_READ);
  if (!f) {
    return 0;
  }
  int n = 0;
  while (f.available()) {
    if (f.read() == '\n') {
      n++;
    }
  }
  f.close();
  return n;
}

static bool wifiConnect(const NetConfig &net) {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.begin(net.ssid.c_str(), net.psk.c_str());
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) {
    delay(50);
  }
  return WiFi.status() == WL_CONNECTED;
}

// The RTC keeps time through deep sleep, so NTP is only needed until it's set.
static void syncClock() {
  if (time(nullptr) >= MIN_VALID_EPOCH) {
    return;
  }
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  uint32_t start = millis();
  while (time(nullptr) < MIN_VALID_EPOCH && millis() - start < NTP_TIMEOUT_MS) {
    delay(50);
  }
}

// Uploads one batch from the head of the queue. On success, keeps only the rest.
static bool uploadBatch(const NetConfig &net, SyncResult &result, bool &more) {
  more = false;
  File f = LittleFS.open(QUEUE_PATH, FILE_READ);
  if (!f) {
    return true;
  }
  String batch;
  int lines = 0;
  while (f.available() && batch.length() < BATCH_BYTES) {
    String line = f.readStringUntil('\n');
    batch += line;
    batch += '\n';
    lines++;
  }
  size_t resume = f.position();
  more = f.available();
  f.close();
  if (lines == 0) {
    return true;
  }

  HTTPClient http;
  String url = "http://" + net.host + ":" + String(net.port) + "/truck/log";
  http.begin(url);
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.addHeader("Content-Type", "application/x-ndjson");
  http.addHeader("X-Truck-Token", net.token);
  result.httpCode = http.POST(batch);
  http.end();
  if (result.httpCode != 200) {
    more = false;
    return false;
  }
  result.sent += lines;

  if (!more) {
    LittleFS.remove(QUEUE_PATH);
    return true;
  }
  File in = LittleFS.open(QUEUE_PATH, FILE_READ);
  File out = LittleFS.open(QUEUE_TMP, FILE_WRITE);
  if (!in || !out) {
    more = false;
    return false;
  }
  in.seek(resume);
  uint8_t buf[512];
  while (in.available()) {
    size_t n = in.read(buf, sizeof(buf));
    out.write(buf, n);
  }
  in.close();
  out.close();
  // If this fails the queue is kept, and the collector ignores the resend.
  LittleFS.remove(QUEUE_PATH);
  LittleFS.rename(QUEUE_TMP, QUEUE_PATH);
  return true;
}

static SyncResult syncQueue(const NetConfig &net) {
  SyncResult result;
  uint32_t start = millis();
  if (net.valid() && wifiConnect(net)) {
    result.wifi = true;
    syncClock();
    bool more = true;
    while (more && millis() - start < UPLOAD_BUDGET_MS) {
      if (!uploadBatch(net, result, more)) {
        break;
      }
    }
  }
  result.ms = millis() - start;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  return result;
}

#if ENABLE_OLED
Adafruit_SSD1306 oled(128, 64, &Wire1, PIN_OLED_RST);

static bool oledBegin() {
  gpio_hold_dis(PIN_VEXT);
  pinMode(PIN_VEXT, OUTPUT);
  digitalWrite(PIN_VEXT, LOW);
  delay(50);
  Wire1.begin(PIN_OLED_SDA, PIN_OLED_SCL);
  return oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
}

// Shows sensor status and readings, then powers the screen down for sleep.
static void oledShow(bool inaOk, bool ahtOk, bool bmpOk, float busV, float currentMa,
                     float tempC, float humidity, float tempBmp, float pressure,
                     const SyncResult &sync, int queued) {
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(0, 0);
  oled.printf("TRUCK seq %lu\n", (unsigned long)seq);
  if (inaOk) {
    oled.printf("INA %.2fV %.1fmA\n", busV, currentMa);
  } else {
    oled.println("INA FAIL");
  }
  if (ahtOk) {
    oled.printf("AHT %.1fF %.0f%%RH\n", tempC * 1.8f + 32.0f, humidity);
  } else {
    oled.println("AHT FAIL");
  }
  if (bmpOk) {
    oled.printf("BMP %.1fF\n%.2f inHg\n", tempBmp * 1.8f + 32.0f, pressure * 0.02953f);
  } else {
    oled.println("BMP FAIL");
  }
  if (!sync.wifi) {
    oled.printf("WiFi DOWN  q=%d\n", queued);
  } else if (sync.httpCode == 200) {
    oled.printf("SYNC OK +%d q=%d\n", sync.sent, queued);
  } else {
    oled.printf("SYNC ERR %d q=%d\n", sync.httpCode, queued);
  }
  oled.display();
  delay(OLED_HOLD_MS);
  oled.ssd1306_command(SSD1306_DISPLAYOFF);
}

static void oledPowerOff() {
  digitalWrite(PIN_VEXT, HIGH);
  gpio_hold_en(PIN_VEXT);
  gpio_deep_sleep_hold_en();
}
#endif

void setup() {
  bool timerWake = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER;
  if (!timerWake || bootId == 0) {
    bootId = (esp_random() >> 1) | 1;
    seq = 0;
  }
  seq++;

  Serial.begin(115200);
  delay(100);
  if (!timerWake) {
    configWindow();
  }

  Wire.begin(PIN_SDA, PIN_SCL, 100000);
#if ENABLE_OLED
  bool oledOk = oledBegin();
#else
  bool oledOk = false;
#endif

  bool fsOk = LittleFS.begin(true);
  bool inaOk = ina219.begin(&Wire);
  bool ahtOk = aht.begin(&Wire);
  bool bmpOk = bmp.begin(BMP280_ADDR);

  float busV = NAN, currentMa = NAN;
  float tempC = NAN, humidity = NAN, tempBmp = NAN, pressure = NAN;

  if (inaOk) {
    busV = ina219.getBusVoltage_V();
    currentMa = ina219.getCurrent_mA();
  }
  if (ahtOk) {
    sensors_event_t h, t;
    if (aht.getEvent(&h, &t)) {
      tempC = t.temperature;
      humidity = h.relative_humidity;
    }
  }
  if (bmpOk) {
    tempBmp = bmp.readTemperature();
    pressure = bmp.readPressure() / 100.0f;
  }

  // Timestamp stays null until the clock has been set by NTP.
  time_t now = time(nullptr);
  String record = "{\"device\":\"";
  record += DEVICE_ID;
  record += "\",\"boot_epoch\":";
  record += String(bootId);
  record += ",\"seq\":";
  record += String(seq);
  record += ",\"timestamp\":";
  record += now >= MIN_VALID_EPOCH ? String((unsigned long)now) : String("null");
  record += ',';
  appendField(record, "voltage", busV, 3);
  appendField(record, "current", currentMa, 1);
  appendField(record, "temp", tempC, 2);
  appendField(record, "humidity", humidity, 1);
  appendField(record, "temp_bmp", tempBmp, 2);
  appendField(record, "pressure", pressure, 2);
  record += "\"synced\":false}";

  bool stored = fsOk && enqueue(record);
  Serial.println(record);

  SyncResult sync = syncQueue(loadNet());
  int queued = queuedRecords();
  Serial.printf("# seq=%lu timer=%d stored=%d wifi=%d http=%d sent=%d queued=%d sync_ms=%lu oled=%d\n",
                (unsigned long)seq, timerWake, stored, sync.wifi, sync.httpCode, sync.sent,
                queued, (unsigned long)sync.ms, oledOk);
  Serial.flush();

#if ENABLE_OLED
  if (oledOk) {
    oledShow(inaOk, ahtOk, bmpOk, busV, currentMa, tempC, humidity, tempBmp, pressure,
             sync, queued);
  }
  oledPowerOff();
#endif

  // Quiet the sensors that would otherwise keep drawing during sleep.
  if (inaOk) {
    ina219.powerSave(true);
  }
  if (bmpOk) {
    bmp.setSampling(Adafruit_BMP280::MODE_SLEEP);
  }

  esp_sleep_enable_timer_wakeup(SLEEP_SECONDS * 1000000ULL);
  esp_deep_sleep_start();
}

void loop() {
  // Not reached: every wake restarts at setup().
}
