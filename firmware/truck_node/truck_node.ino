// Truck Monitor sensor node with LoRa sync for the Heltec WiFi LoRa32 V3.
// Each wake: read sensors, append the record to flash, send what is queued to the
// home gateway, then deep sleep. A record leaves flash only after the gateway acks
// it, which it does once CT100 has confirmed it. Each ack also sets the clock.
// Sensors sit on GPIO41/42, separate from the OLED bus on GPIO17/18.
//
// The link key lives in NVS, set over USB within CFG_WINDOW_MS of a reset:
//   cfg<TAB>ssid<TAB>psk<TAB>host<TAB>port<TAB>token
//   cfg<TAB>ssid<TAB>psk<TAB>host<TAB>port<TAB>token<TAB>otakey<TAB>ssid2<TAB>psk2
// See tools/set_net.py. The token keys the link. ssid/psk and ssid2/psk2 are the
// networks tried for an update, and otakey checks its signature. None are printed.
//
// Updates: an ack can name a waiting update, or RST then holding PRG for 5 s asks
// for one by hand. WiFi comes on only then. The image must be signed by the key on
// the build machine, so the HTTPS download itself does not need to be trusted. New
// firmware must complete a LoRa sync on its first wake or it rolls back, and a
// crash before that makes the bootloader roll back too.

#include "lora_link.h"
#include <Wire.h>
#include <esp_sleep.h>
#include <esp_random.h>
#include <sys/time.h>
#include <time.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <mbedtls/sha256.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <Adafruit_INA219.h>
#include <Adafruit_AHTX0.h>
#include <Adafruit_BMP280.h>
#include <Adafruit_SSD1306.h>
#include <RadioLib.h>
#include <driver/gpio.h>

// Bench diagnostics screen. Set to 0 for the truck to save battery.
#ifndef ENABLE_OLED
#define ENABLE_OLED 1
#endif
#ifndef BUILD_MARK
#define BUILD_MARK 0
#endif
#ifndef WAKE_SECONDS
#define WAKE_SECONDS 20
#endif

static const char *DEVICE_ID = "truck1";
static const int PIN_SDA = 41;
static const int PIN_SCL = 42;
static const uint8_t BMP280_ADDR = 0x76;
static const uint64_t SLEEP_SECONDS = WAKE_SECONDS;

static const char *QUEUE_PATH = "/queue.bin";
static const char *QUEUE_TMP = "/queue.tmp";
static const uint32_t ACK_WAIT_MS = 3000;
static const uint8_t SEND_TRIES = 3;
static const uint32_t UPLOAD_BUDGET_MS = 15000;
static const char *OTA_ORDER_URL = "https://repairmanray.com/api/truck/order";
static const char *OTA_FW_URL = "https://repairmanray.com/api/truck/fw";
static const uint32_t OTA_WIFI_MS = 30000;
static const uint32_t OTA_READ_MS = 15000;
static const uint8_t OTA_MAX_TRIES = 6;
static const int PIN_PRG = 0;
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
static uint32_t runningOtaId = 0;

Adafruit_INA219 ina219;
Adafruit_AHTX0 aht;
Adafruit_BMP280 bmp(&Wire);

struct NetConfig {
  String ssid, psk, host, token, otaKey, ssid2, psk2;
  int port = 0;
};

struct SyncResult {
  bool link = false;   // At least one authentic ack arrived.
  bool stored = true;  // False if the gateway reported a failed store.
  int sent = 0;
  uint32_t otaId = 0;  // Update the gateway says is waiting.
  int rssi = 0;
  float snr = 0;
  uint32_t ms = 0;
};

SX1262 radio = new Module(PIN_LORA_NSS, PIN_LORA_DIO1, PIN_LORA_RST, PIN_LORA_BUSY);
volatile bool dio1Fired = false;
static void IRAM_ATTR onDio1() { dio1Fired = true; }

static NetConfig loadNet() {
  NetConfig n;
  Preferences p;
  if (p.begin("net", true)) {
    n.ssid = p.getString("ssid", "");
    n.psk = p.getString("psk", "");
    n.host = p.getString("host", "");
    n.token = p.getString("token", "");
    n.port = p.getInt("port", 0);
    n.otaKey = p.getString("otakey", "");
    n.ssid2 = p.getString("ssid2", "");
    n.psk2 = p.getString("psk2", "");
    p.end();
  }
  return n;
}

static bool saveNet(const String &line) {
  String part[8];
  int start = line.indexOf('\t') + 1;
  int fields = 0;
  while (fields < 8) {
    int tab = line.indexOf('\t', start);
    part[fields++] = tab < 0 ? line.substring(start) : line.substring(start, tab);
    if (tab < 0) {
      break;
    }
    start = tab + 1;
  }
  if (fields < 5) {
    return false;
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
  if (fields == 8) {
    p.putString("otakey", part[5]);
    p.putString("ssid2", part[6]);
    p.putString("psk2", part[7]);
  }
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

static uint16_t scaleU16(float value, float scale) {
  return isnan(value) ? NO_U16 : (uint16_t)constrain(lroundf(value * scale), 0L, 65534L);
}

static int16_t scaleI16(float value, float scale) {
  return isnan(value) ? NO_I16 : (int16_t)constrain(lroundf(value * scale), -32767L, 32767L);
}

static bool enqueue(const LinkRecord &record) {
  File f = LittleFS.open(QUEUE_PATH, FILE_APPEND);
  if (!f) {
    return false;
  }
  // Pads out a write cut short by power loss so later records stay aligned. The
  // padded slot is stale, and a zero bootId tells the gateway to skip it.
  size_t partial = f.size() % sizeof(LinkRecord);
  if (partial != 0) {
    LinkRecord blank = {};
    f.write((const uint8_t *)&blank, sizeof(blank) - partial);
  }
  bool ok = f.write((const uint8_t *)&record, sizeof(record)) == sizeof(record);
  f.close();
  return ok;
}

static int queuedRecords() {
  File f = LittleFS.open(QUEUE_PATH, FILE_READ);
  if (!f) {
    return 0;
  }
  int n = f.size() / sizeof(LinkRecord);
  f.close();
  return n;
}

static bool radioBegin() {
  gpio_hold_dis((gpio_num_t)PIN_LORA_NSS);
  SPI.begin(PIN_LORA_SCK, PIN_LORA_MISO, PIN_LORA_MOSI, PIN_LORA_NSS);
  int rc = radio.begin(LINK_FREQ_MHZ, LINK_BW_KHZ, LINK_SF, LINK_CR, LINK_SYNC, LINK_DBM,
                       LINK_PREAMBLE, LINK_TCXO_V);
  if (rc == RADIOLIB_ERR_NONE) rc = radio.setDio2AsRfSwitch(true);
  if (rc == RADIOLIB_ERR_NONE) rc = radio.setCurrentLimit(140);
  if (rc != RADIOLIB_ERR_NONE) {
    Serial.printf("# radio error %d\n", rc);
    return false;
  }
  radio.setPacketReceivedAction(onDio1);
  return true;
}

// Tells the core not to approve new firmware by itself, so setup() can decide.
extern "C" bool verifyRollbackLater() { return true; }

static uint32_t otaGet(const char *key) {
  Preferences p;
  uint32_t value = 0;
  if (p.begin("ota", true)) {
    value = p.getUInt(key, 0);
    p.end();
  }
  return value;
}

static void otaPut(const char *key, uint32_t value) {
  Preferences p;
  if (p.begin("ota", false)) {
    p.putUInt(key, value);
    p.end();
  }
}

static bool onTrial() {
  esp_ota_img_states_t state;
  return esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
         state == ESP_OTA_IMG_PENDING_VERIFY;
}

static void hexOf(const uint8_t *in, size_t len, char *out) {
  for (size_t i = 0; i < len; i++) {
    sprintf(out + i * 2, "%02x", in[i]);
  }
}

static bool otaOrderOk(const char *key, const char *sha, const char *mac) {
  uint8_t full[32];
  char hex[65];
  mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), (const uint8_t *)key, strlen(key),
                  (const uint8_t *)sha, 64, full);
  hexOf(full, sizeof(full), hex);
  uint8_t diff = 0;
  for (int i = 0; i < 64; i++) {
    diff |= hex[i] ^ mac[i];
  }
  return diff == 0;
}

// Streams the image into the idle slot, checking its size and sha256 before use.
static bool otaInstall(WiFiClientSecure &tls, uint32_t size, const char *sha) {
  HTTPClient http;
  http.begin(tls, OTA_FW_URL);
  http.setTimeout(OTA_READ_MS);
  int code = http.GET();
  if (code != 200 || http.getSize() != (int)size) {
    Serial.printf("# ota download failed http=%d size=%d\n", code, http.getSize());
    http.end();
    return false;
  }
  if (!Update.begin(size)) {
    Serial.printf("# ota cannot start: %s\n", Update.errorString());
    http.end();
    return false;
  }
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  WiFiClient *stream = http.getStreamPtr();
  static uint8_t chunk[1024];
  uint32_t got = 0, last = millis();
  while (got < size && millis() - last < OTA_READ_MS) {
    int ready = stream->available();
    if (ready <= 0) {
      delay(2);
      continue;
    }
    int n = stream->read(chunk, min((size_t)ready, sizeof(chunk)));
    if (n <= 0) {
      continue;
    }
    mbedtls_sha256_update(&ctx, chunk, n);
    if (Update.write(chunk, n) != (size_t)n) {
      break;
    }
    got += n;
    last = millis();
  }
  http.end();
  uint8_t digest[32];
  char hex[65];
  mbedtls_sha256_finish(&ctx, digest);
  mbedtls_sha256_free(&ctx);
  hexOf(digest, sizeof(digest), hex);
  if (got != size || strncmp(hex, sha, 64) != 0) {
    Update.abort();
    Serial.printf("# ota image failed its check, got %lu of %lu\n", (unsigned long)got,
                  (unsigned long)size);
    return false;
  }
  if (!Update.end(true)) {
    Serial.printf("# ota image rejected: %s\n", Update.errorString());
    return false;
  }
  return true;
}

// Fetches the signed order and installs it. Returns only if nothing was installed.
static void otaRun(const NetConfig &net, uint32_t wantedId, bool byHand) {
  if (net.otaKey.length() < 16) {
    Serial.println("# ota no key set");
    return;
  }
  WiFiMulti multi;
  if (net.ssid2.length()) {
    multi.addAP(net.ssid2.c_str(), net.psk2.c_str());
  }
  if (net.ssid.length()) {
    multi.addAP(net.ssid.c_str(), net.psk.c_str());
  }
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  bool up = multi.run(OTA_WIFI_MS) == WL_CONNECTED;
  Serial.printf("# ota wifi=%d rssi=%d\n", up, up ? WiFi.RSSI() : 0);
  if (up) {
    WiFiClientSecure tls;
    tls.setInsecure();  // Trust comes from the signature, and no secret is sent.
    HTTPClient http;
    http.begin(tls, OTA_ORDER_URL);
    http.setTimeout(OTA_READ_MS);
    int code = http.GET();
    String order = code == 200 ? http.getString() : String();
    http.end();
    unsigned long size = 0;
    char sha[65] = "", mac[65] = "";
    if (sscanf(order.c_str(), "%lu %64s %64s", &size, sha, mac) != 3 || strlen(sha) != 64 ||
        strlen(mac) != 64 || size < 100000) {
      Serial.printf("# ota no usable order http=%d\n", code);
    } else if (!otaOrderOk(net.otaKey.c_str(), sha, mac)) {
      Serial.println("# ota refused a bad signature");
    } else {
      char head[9] = "";
      strncpy(head, sha, 8);
      uint32_t id = strtoul(head, nullptr, 16);
      if (id != wantedId && !byHand) {
        Serial.println("# ota order changed, waiting for the next ack");
      } else if (id == otaGet("seen") && !byHand) {
        Serial.println("# ota already tried this one");
      } else {
        Serial.printf("# ota installing %08lx, %lu bytes\n", (unsigned long)id, size);
        if (otaInstall(tls, size, sha)) {
          otaPut("seen", id);
          otaPut("next", id);
          otaPut("tries", 0);
          Serial.println("# ota installed, restarting");
          Serial.flush();
          ESP.restart();
        }
      }
    }
  }
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

// Sleeps the radio and holds its select line so it stays asleep in deep sleep.
static void radioPowerOff() {
  radio.sleep(false);
  pinMode(PIN_LORA_NSS, OUTPUT);
  digitalWrite(PIN_LORA_NSS, HIGH);
  gpio_hold_en((gpio_num_t)PIN_LORA_NSS);
  gpio_deep_sleep_hold_en();
}

// The gateway's clock replaces NTP, which also corrects drift on every ack.
static void setClock(uint32_t epoch) {
  if (epoch >= MIN_VALID_EPOCH) {
    timeval tv = {(time_t)epoch, 0};
    settimeofday(&tv, nullptr);
  }
}

// Sends one frame and waits for its ack, retrying on silence.
static bool sendFrame(uint8_t *frame, size_t len, const String &key, SyncResult &result) {
  uint32_t nonce = esp_random();
  memcpy(frame + 4, &nonce, sizeof(nonce));
  memcpy(frame + 8, &runningOtaId, sizeof(runningOtaId));
  linkTag(key.c_str(), frame, len - LINK_TAG_BYTES, frame + len - LINK_TAG_BYTES);

  for (uint8_t attempt = 0; attempt < SEND_TRIES; attempt++) {
    if (radio.transmit(frame, len) != RADIOLIB_ERR_NONE) {
      continue;
    }
    dio1Fired = false;
    radio.startReceive();
    uint32_t start = millis();
    while (millis() - start < ACK_WAIT_MS) {
      if (!dio1Fired) {
        delay(2);
        continue;
      }
      dio1Fired = false;
      AckFrame ack;
      size_t got = radio.getPacketLength();
      int rc = got == sizeof(ack) ? radio.readData((uint8_t *)&ack, sizeof(ack)) : RADIOLIB_ERR_UNKNOWN;
      if (rc == RADIOLIB_ERR_NONE && ack.a == 'T' && ack.b == 'L' && ack.type == FRAME_ACK &&
          ack.nonce == nonce &&
          linkTagOk(key.c_str(), (const uint8_t *)&ack, sizeof(ack) - LINK_TAG_BYTES, ack.tag)) {
        result.link = true;
        result.rssi = (int)radio.getRSSI();
        result.snr = radio.getSNR();
        setClock(ack.epoch);
        result.otaId = ack.otaId;
        radio.standby();
        result.stored = ack.status == ACK_STORED;
        return result.stored;
      }
      radio.startReceive();
    }
    radio.standby();
  }
  return false;
}

// Drops the acked records from the head of the queue.
static void trimQueue(size_t acked) {
  if (acked == 0) {
    return;
  }
  File in = LittleFS.open(QUEUE_PATH, FILE_READ);
  if (!in) {
    return;
  }
  if (acked >= in.size()) {
    in.close();
    LittleFS.remove(QUEUE_PATH);
    return;
  }
  File out = LittleFS.open(QUEUE_TMP, FILE_WRITE);
  if (!out) {
    in.close();
    return;
  }
  in.seek(acked);
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
}

static SyncResult syncQueue(const String &key, bool radioOk) {
  SyncResult result;
  uint32_t start = millis();
  File f = LittleFS.open(QUEUE_PATH, FILE_READ);
  if (f && radioOk && key.length() > 0) {
    uint8_t frame[MAX_DATA_BYTES];
    size_t acked = 0;
    while (millis() - start < UPLOAD_BUDGET_MS) {
      f.seek(acked);
      size_t got = f.read(frame + DATA_HEADER_BYTES, MAX_RECORDS * sizeof(LinkRecord));
      uint8_t count = got / sizeof(LinkRecord);
      if (count == 0) {
        break;
      }
      frame[0] = 'T';
      frame[1] = 'L';
      frame[2] = FRAME_DATA;
      frame[3] = count;
      size_t len = DATA_HEADER_BYTES + count * sizeof(LinkRecord) + LINK_TAG_BYTES;
      if (!sendFrame(frame, len, key, result)) {
        break;
      }
      acked += count * sizeof(LinkRecord);
      result.sent += count;
    }
    f.close();
    trimQueue(acked);
  } else if (f) {
    f.close();
  }
  result.ms = millis() - start;
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
  if (!sync.link) {
    oled.printf("LoRa NO ACK q=%d\n", queued);
  } else if (sync.stored) {
    oled.printf("SYNC OK +%d q=%d\n", sync.sent, queued);
  } else {
    oled.printf("STORE FAIL q=%d\n", queued);
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
#ifdef TEST_CRASH_ON_BOOT
  if (onTrial()) {
    Serial.println("# test build crashing");
    abort();
  }
#endif
  bool otaByHand = false;
  if (!timerWake) {
    configWindow();
    pinMode(PIN_PRG, INPUT_PULLUP);
    delay(10);
    otaByHand = digitalRead(PIN_PRG) == LOW;
  }
  bool trial = onTrial();
  runningOtaId = otaGet(trial ? "next" : "cur");
  Serial.printf("# fw %08lx trial=%d test=%d\n", (unsigned long)runningOtaId, trial, BUILD_MARK);

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

  // Timestamp stays 0 until the first ack has set the clock.
  time_t now = time(nullptr);
  LinkRecord record = {};
  record.bootId = bootId;
  record.seq = seq;
  record.timestamp = now >= MIN_VALID_EPOCH ? (uint32_t)now : 0;
  record.millivolts = scaleU16(busV, 1000.0f);
  record.currentX10 = scaleI16(currentMa, 10.0f);
  record.tempX100 = scaleI16(tempC, 100.0f);
  record.humidityX10 = scaleU16(humidity, 10.0f);
  record.tempBmpX100 = scaleI16(tempBmp, 100.0f);
  record.pressureX10 = scaleU16(pressure, 10.0f);

  bool stored = fsOk && enqueue(record);
  Serial.printf("{\"seq\":%lu,\"timestamp\":%lu,\"voltage\":%.3f,\"current\":%.1f,\"temp\":%.2f,\"humidity\":%.1f,\"temp_bmp\":%.2f,\"pressure\":%.1f}\n",
                (unsigned long)seq, (unsigned long)record.timestamp, busV, currentMa, tempC,
                humidity, tempBmp, pressure);

  bool radioOk = radioBegin();
  NetConfig net = loadNet();
#ifdef TEST_NO_SYNC
  SyncResult sync = syncQueue(trial ? String() : net.token, radioOk);
#else
  SyncResult sync = syncQueue(net.token, radioOk);
#endif
  if (trial) {
    if (sync.link && sync.stored) {
      otaPut("cur", runningOtaId);
      esp_ota_mark_app_valid_cancel_rollback();
      Serial.println("# ota confirmed");
    } else {
      Serial.println("# ota no sync on trial, rolling back");
      Serial.flush();
      esp_ota_mark_app_invalid_rollback_and_reboot();
    }
  } else {
    uint32_t tries = otaGet("tries");
    bool wanted = sync.otaId != 0 && sync.otaId != otaGet("seen");
    if (!wanted && sync.otaId == 0 && tries != 0) {
      otaPut("tries", 0);
    }
    if (otaByHand || (wanted && tries < OTA_MAX_TRIES)) {
      if (radioOk) {
        radio.standby();
      }
      otaPut("tries", tries + 1);
      otaRun(net, sync.otaId, otaByHand);
    }
  }
  int queued = queuedRecords();
  Serial.printf("# seq=%lu timer=%d stored=%d link=%d ok=%d sent=%d queued=%d rssi=%d snr=%.2f sync_ms=%lu oled=%d\n",
                (unsigned long)seq, timerWake, stored, sync.link, sync.stored, sync.sent, queued,
                sync.rssi, sync.snr, (unsigned long)sync.ms, oledOk);
  Serial.flush();

#if ENABLE_OLED
  if (oledOk) {
    oledShow(inaOk, ahtOk, bmpOk, busV, currentMa, tempC, humidity, tempBmp, pressure,
             sync, queued);
  }
  oledPowerOff();
#endif

  // Quiet the radio and sensors that would otherwise keep drawing during sleep.
  if (radioOk) {
    radioPowerOff();
  }
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
