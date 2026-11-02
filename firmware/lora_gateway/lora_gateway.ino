// Truck Monitor home gateway for a Heltec WiFi LoRa32 V3 on wall power.
// Receives signed record frames over LoRa, posts them to the CT100 collector over
// WiFi, and acks only once the collector has answered 200, so the truck keeps
// anything that did not get stored.
//
// Network settings live in NVS, set over USB within CFG_WINDOW_MS of a reset:
//   cfg<TAB>ssid<TAB>psk<TAB>host<TAB>port<TAB>token
// See tools/set_net.py. They are never printed.

#include "lora_link.h"
#include <Wire.h>
#include <SPI.h>
#include <time.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <RadioLib.h>
#include <Adafruit_SSD1306.h>
#include <esp_random.h>
#include <esp_task_wdt.h>

static const char *DEVICE_ID = "truck1";
static const uint32_t CFG_WINDOW_MS = 4000;
static const uint32_t HTTP_CONNECT_MS = 800;
static const uint32_t HTTP_TIMEOUT_MS = 1500;
static const uint32_t WIFI_RESTART_MS = 300000;
static const uint32_t STATUS_MS = 30000;
static const uint32_t QUIET_RESTART_MS = 3600000;  // Well past the truck's wake interval.
static const uint32_t LOOP_WATCHDOG_MS = 60000;
static const time_t MIN_VALID_EPOCH = 1600000000;

static const int PIN_OLED_SDA = 17, PIN_OLED_SCL = 18, PIN_OLED_RST = 21, PIN_VEXT = 36;

SX1262 radio = new Module(PIN_LORA_NSS, PIN_LORA_DIO1, PIN_LORA_RST, PIN_LORA_BUSY);
Adafruit_SSD1306 oled(128, 64, &Wire1, PIN_OLED_RST);
volatile bool dio1Fired = false;
static void IRAM_ATTR onDio1() { dio1Fired = true; }

static String ssid, psk, host, token;
static int port = 0;
static uint32_t wifiLostAt = 0;
static uint32_t lastStatus = 0;
static uint32_t lastFrameAt = 0;
static uint32_t framesOk = 0, framesBad = 0, recordsOk = 0;
static int lastHttp = 0, lastRssi = 0;
static uint32_t otaWaiting = 0;
static float lastSnr = 0;

static void loadNet() {
  Preferences p;
  if (p.begin("net", true)) {
    ssid = p.getString("ssid", "");
    psk = p.getString("psk", "");
    host = p.getString("host", "");
    token = p.getString("token", "");
    port = p.getInt("port", 0);
    p.end();
  }
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

static void configWindow() {
  Serial.println("# cfg window");
  Serial.setTimeout(CFG_WINDOW_MS);
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.startsWith("cfg\t")) {
    Serial.println(saveNet(line) ? "# cfg saved" : "# cfg rejected");
  }
}

static void showStatus() {
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(0, 0);
  oled.printf("GATEWAY SF%d\n", LINK_SF);
  if (WiFi.status() == WL_CONNECTED) {
    oled.printf("WiFi %d dBm\n", WiFi.RSSI());
  } else {
    oled.println("WiFi DOWN");
  }
  oled.printf("Records %lu\n", (unsigned long)recordsOk);
  oled.printf("Frames %lu bad %lu\n", (unsigned long)framesOk, (unsigned long)framesBad);
  oled.printf("HTTP %d\n", lastHttp);
  oled.printf("LoRa %d/%.0f\n", lastRssi, lastSnr);
  oled.display();
}

static void field(String &out, const char *name, int raw, int missing, float scale, int decimals) {
  out += ",\"";
  out += name;
  out += "\":";
  if (raw == missing) {
    out += "null";
  } else {
    out += String(raw / scale, decimals);
  }
}

static String recordJson(const LinkRecord &r, uint32_t fw) {
  String out = "{\"device\":\"";
  out += DEVICE_ID;
  out += "\",\"boot_epoch\":";
  out += String(r.bootId);
  out += ",\"seq\":";
  out += String(r.seq);
  out += ",\"timestamp\":";
  out += r.timestamp ? String(r.timestamp) : String("null");
  field(out, "voltage", r.millivolts, NO_U16, 1000.0f, 3);
  field(out, "current", r.currentX10, NO_I16, 10.0f, 1);
  field(out, "temp", r.tempX100, NO_I16, 100.0f, 2);
  field(out, "humidity", r.humidityX10, NO_U16, 10.0f, 1);
  field(out, "temp_bmp", r.tempBmpX100, NO_I16, 100.0f, 2);
  field(out, "pressure", r.pressureX10, NO_U16, 10.0f, 1);
  out += ",\"fw\":";
  out += String(fw);
  out += "}\n";
  return out;
}

static int postRecords(const String &body) {
  if (WiFi.status() != WL_CONNECTED) {
    return 0;
  }
  HTTPClient http;
  http.begin("http://" + host + ":" + String(port) + "/truck/log");
  http.setConnectTimeout(HTTP_CONNECT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.addHeader("Content-Type", "application/x-ndjson");
  http.addHeader("X-Truck-Token", token);
  int code = http.POST(body);
  if (code == 200) {
    // The reply names any update waiting for the truck.
    String reply = http.getString();
    int at = reply.indexOf("\"ota\":");
    otaWaiting = at < 0 ? 0 : strtoul(reply.c_str() + at + 6, nullptr, 10);
  }
  http.end();
  return code;
}

static void sendAck(uint32_t nonce, uint8_t status) {
  time_t now = time(nullptr);
  AckFrame ack = {'T', 'L', FRAME_ACK, status, nonce, now >= MIN_VALID_EPOCH ? (uint32_t)now : 0,
                  otaWaiting, {0}};
  linkTag(token.c_str(), (const uint8_t *)&ack, sizeof(ack) - LINK_TAG_BYTES, ack.tag);
  int rc = radio.transmit((uint8_t *)&ack, sizeof(ack));
  if (rc != RADIOLIB_ERR_NONE) {
    Serial.printf("# ack tx error %d\n", rc);
  }
}

static void handleFrame() {
  uint8_t buf[256];
  size_t len = radio.getPacketLength();
  int rc = radio.readData(buf, min(len, sizeof(buf)));
  lastRssi = (int)radio.getRSSI();
  lastSnr = radio.getSNR();
  if (rc != RADIOLIB_ERR_NONE || len < DATA_HEADER_BYTES + LINK_TAG_BYTES || len > MAX_DATA_BYTES ||
      buf[0] != 'T' || buf[1] != 'L' || buf[2] != FRAME_DATA) {
    framesBad++;
    Serial.printf("# bad frame rc=%d len=%u\n", rc, (unsigned)len);
    return;
  }
  uint8_t count = buf[3];
  size_t want = DATA_HEADER_BYTES + count * sizeof(LinkRecord) + LINK_TAG_BYTES;
  if (count == 0 || count > MAX_RECORDS || len != want ||
      !linkTagOk(token.c_str(), buf, len - LINK_TAG_BYTES, buf + len - LINK_TAG_BYTES)) {
    framesBad++;
    Serial.printf("# rejected frame len=%u count=%u\n", (unsigned)len, count);
    return;
  }

  uint32_t nonce, fw;
  memcpy(&nonce, buf + 4, sizeof(nonce));
  memcpy(&fw, buf + 8, sizeof(fw));
  String body;
  int stored = 0;
  for (uint8_t i = 0; i < count; i++) {
    LinkRecord r;
    memcpy(&r, buf + DATA_HEADER_BYTES + i * sizeof(LinkRecord), sizeof(r));
    if (r.bootId != 0) {
      body += recordJson(r, fw);
      stored++;
    }
  }
  lastHttp = stored ? postRecords(body) : 200;
  bool ok = lastHttp == 200;
  sendAck(nonce, ok ? ACK_STORED : ACK_FAILED);
  framesOk++;
  lastFrameAt = millis();
  if (ok) {
    recordsOk += stored;
  }
  Serial.printf("# frame count=%u http=%d rssi=%d snr=%.2f\n", count, lastHttp, lastRssi, lastSnr);
  showStatus();
}

void setup() {
  Serial.begin(115200);
  delay(100);
  configWindow();
  loadNet();

  pinMode(PIN_VEXT, OUTPUT);
  digitalWrite(PIN_VEXT, LOW);
  delay(100);
  Wire1.begin(PIN_OLED_SDA, PIN_OLED_SCL);
  oled.begin(SSD1306_SWITCHCAPVCC, 0x3C);

  SPI.begin(PIN_LORA_SCK, PIN_LORA_MISO, PIN_LORA_MOSI, PIN_LORA_NSS);
  int rc = radio.begin(LINK_FREQ_MHZ, LINK_BW_KHZ, LINK_SF, LINK_CR, LINK_SYNC, LINK_DBM,
                       LINK_PREAMBLE, LINK_TCXO_V);
  if (rc == RADIOLIB_ERR_NONE) rc = radio.setDio2AsRfSwitch(true);
  if (rc == RADIOLIB_ERR_NONE) rc = radio.setCurrentLimit(140);
  Serial.printf("# gateway radio rc=%d sf=%d configured=%d\n", rc, LINK_SF, token.length() > 0);
  if (rc != RADIOLIB_ERR_NONE) {
    delay(30000);
    ESP.restart();
  }
  radio.setPacketReceivedAction(onDio1);

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);
  WiFi.begin(ssid.c_str(), psk.c_str());
  configTime(0, 0, "pool.ntp.org", "time.google.com");

  showStatus();
  radio.startReceive();

  esp_task_wdt_config_t wdt = {LOOP_WATCHDOG_MS, 0, true};
  esp_task_wdt_reconfigure(&wdt);
  esp_task_wdt_add(nullptr);
}

void loop() {
  esp_task_wdt_reset();
  if (dio1Fired) {
    dio1Fired = false;
    handleFrame();
    dio1Fired = false;
    radio.startReceive();
  }

  uint32_t now = millis();
  if (WiFi.status() == WL_CONNECTED) {
    wifiLostAt = 0;
  } else if (wifiLostAt == 0) {
    wifiLostAt = now | 1;
  } else if (now - wifiLostAt > WIFI_RESTART_MS) {
    Serial.println("# wifi down too long, restarting");
    ESP.restart();
  }

  // A deaf radio looks like silence, so a long quiet spell gets a clean restart.
  if (now - lastFrameAt > QUIET_RESTART_MS) {
    Serial.println("# no frames for too long, restarting");
    ESP.restart();
  }

  if (now - lastStatus > STATUS_MS) {
    lastStatus = now;
    Serial.printf("# status wifi=%d rssi=%d clock=%d records=%lu frames=%lu bad=%lu\n",
                  WiFi.status() == WL_CONNECTED, WiFi.RSSI(), time(nullptr) >= MIN_VALID_EPOCH,
                  (unsigned long)recordsOk, (unsigned long)framesOk, (unsigned long)framesBad);
    showStatus();
  }
  delay(5);
}
