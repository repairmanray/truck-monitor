// LoRa range test for two Heltec V3 boards, 915 MHz point-to-point.
// Build with -DROLE_PING=1 for the truck end, -DROLE_PING=0 for the home end.
// The truck end sends a ping every few seconds and the home end answers, so
// each ping tests both directions. Results go to the OLED and serial as CSV.
#include <Wire.h>
#include <SPI.h>
#include <RadioLib.h>
#include <Adafruit_SSD1306.h>
#include "packet.h"

#ifndef ROLE_PING
#error "Define ROLE_PING as 0 or 1"
#endif
#ifndef TX_DBM
#define TX_DBM 2
#endif

static const float FREQ_MHZ = 915.0;
static const float BW_KHZ = 125.0;
static const uint8_t SF = 10;
static const uint8_t CR = 7;
static const uint8_t SYNC = 0x12;  // Private LoRa sync word.
static const uint32_t PING_MS = 4000;
static const uint32_t REPLY_WAIT_MS = 1500;
static const uint8_t MAGIC_A = 'T', MAGIC_B = 'R', T_PING = 1, T_PONG = 2;

static const int PIN_SCK = 9, PIN_MISO = 11, PIN_MOSI = 10, PIN_NSS = 8;
static const int PIN_DIO1 = 14, PIN_RST = 12, PIN_BUSY = 13;
static const int PIN_OLED_SDA = 17, PIN_OLED_SCL = 18, PIN_OLED_RST = 21, PIN_VEXT = 36;

SX1262 radio = new Module(PIN_NSS, PIN_DIO1, PIN_RST, PIN_BUSY);
Adafruit_SSD1306 oled(128, 64, &Wire1, PIN_OLED_RST);
volatile bool dio1Fired = false;
static void IRAM_ATTR onDio1() { dio1Fired = true; }

static uint16_t seqNo = 0;
static uint32_t acked = 0;
static uint32_t recent = 0;  // One bit per ping, newest in bit 0.
static int lastLocalRssi = 0, lastRemoteRssi = 0;
static float lastLocalSnr = 0, lastRemoteSnr = 0;

static void show(const char* line1, const char* line2, const char* line3) {
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(0, 0);
  oled.printf("%s SF%u %ddBm\n\n", ROLE_PING ? "TRUCK" : "HOME", SF, TX_DBM);
  oled.println(line1);
  oled.println(line2);
  oled.println(line3);
  oled.display();
}

static bool sendPacket(const Packet& p) {
  dio1Fired = false;
  int rc = radio.transmit((uint8_t*)&p, sizeof(p));
  if (rc != RADIOLIB_ERR_NONE) Serial.printf("# tx error %d\n", rc);
  return rc == RADIOLIB_ERR_NONE;
}

static bool readPacket(Packet& p) {
  if (radio.getPacketLength() != sizeof(p)) { radio.readData((uint8_t*)&p, 0); return false; }
  int rc = radio.readData((uint8_t*)&p, sizeof(p));
  return rc == RADIOLIB_ERR_NONE && p.a == MAGIC_A && p.b == MAGIC_B;
}

#if ROLE_PING
static void step() {
  Packet out = {MAGIC_A, MAGIC_B, T_PING, ++seqNo, 0, 0};
  uint32_t started = millis();
  bool got = false;
  if (sendPacket(out)) {
    dio1Fired = false;
    radio.startReceive();
    uint32_t waitFrom = millis();
    while (millis() - waitFrom < REPLY_WAIT_MS) {
      if (!dio1Fired) { delay(5); continue; }
      dio1Fired = false;
      Packet in;
      if (readPacket(in) && in.type == T_PONG && in.seq == seqNo) {
        got = true;
        lastLocalRssi = (int)radio.getRSSI();
        lastLocalSnr = radio.getSNR();
        lastRemoteRssi = in.rssi;
        lastRemoteSnr = in.snrX4 / 4.0;
        break;
      }
      radio.startReceive();
    }
    radio.standby();
  }
  recent = (recent << 1) | (got ? 1 : 0);
  if (got) acked++;
  int recentPct = __builtin_popcount(recent & 0xFFFFF) * 100 / min((int)seqNo, 20);
  Serial.printf("%u,%d,%d,%.2f,%d,%.2f,%lu\n", seqNo, got, lastLocalRssi, lastLocalSnr,
                lastRemoteRssi, lastRemoteSnr, (unsigned long)acked);
  char l1[32], l2[32], l3[32];
  snprintf(l1, sizeof(l1), "%s #%u", got ? "ACK" : "MISS", seqNo);
  snprintf(l2, sizeof(l2), "last 20: %d%%", recentPct);
  snprintf(l3, sizeof(l3), "%d/%.0f  %d/%.0f", lastLocalRssi, lastLocalSnr, lastRemoteRssi, lastRemoteSnr);
  show(l1, l2, l3);
  uint32_t used = millis() - started;
  if (used < PING_MS) delay(PING_MS - used);
}
#else
static void step() {
  if (!dio1Fired) { delay(5); return; }
  dio1Fired = false;
  Packet in;
  if (!readPacket(in) || in.type != T_PING) { radio.startReceive(); return; }
  int rssi = (int)radio.getRSSI();
  float snr = radio.getSNR();
  Packet out = {MAGIC_A, MAGIC_B, T_PONG, in.seq, (int16_t)rssi, (int8_t)(snr * 4)};
  sendPacket(out);
  acked++;
  Serial.printf("%u,%d,%.2f,%lu\n", in.seq, rssi, snr, (unsigned long)acked);
  char l1[32], l2[32], l3[32];
  snprintf(l1, sizeof(l1), "Heard #%u", in.seq);
  snprintf(l2, sizeof(l2), "total %lu", (unsigned long)acked);
  snprintf(l3, sizeof(l3), "RSSI %d SNR %.0f", rssi, snr);
  show(l1, l2, l3);
  dio1Fired = false;
  radio.startReceive();
}
#endif

void setup() {
  Serial.begin(115200);
  pinMode(PIN_VEXT, OUTPUT);
  digitalWrite(PIN_VEXT, LOW);
  delay(100);
  Wire1.begin(PIN_OLED_SDA, PIN_OLED_SCL);
  oled.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  show("Starting", "", "");

  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_NSS);
  int rc = radio.begin(FREQ_MHZ, BW_KHZ, SF, CR, SYNC, TX_DBM, 12, 1.6);
  if (rc == RADIOLIB_ERR_NONE) rc = radio.setDio2AsRfSwitch(true);
  if (rc == RADIOLIB_ERR_NONE) rc = radio.setCurrentLimit(140);
  Serial.printf("# radio rc=%d role=%s dbm=%d\n", rc, ROLE_PING ? "truck" : "home", TX_DBM);
  if (rc != RADIOLIB_ERR_NONE) {
    show("RADIO FAIL", "", "");
    while (true) delay(1000);
  }
  radio.setPacketReceivedAction(onDio1);
#if !ROLE_PING
  radio.startReceive();
  show("Listening", "", "");
#endif
}

void loop() { step(); }
