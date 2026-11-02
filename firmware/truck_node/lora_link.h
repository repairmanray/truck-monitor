// Radio settings and frame format shared by truck_node and lora_gateway.
// Keep the two copies of this file identical.
//
// Data frame: 'T' 'L' 1 count nonce32 fw32 record[count] tag8
// Ack frame:  'T' 'L' 2 status nonce32 epoch32 ota32 tag8
// fw32 is the update the truck is running (0 = USB build). ota32 is the update
// waiting on the server (0 = none). Both are the first 4 bytes of its sha256.
// The tag is a truncated HMAC-SHA256 keyed with the collector token, so nothing
// nearby can inject records or fake an ack. Payloads are signed, not encrypted.
#pragma once

#include <stdint.h>
#include <string.h>
#include <math.h>
#include <mbedtls/md.h>

static const float LINK_FREQ_MHZ = 915.0;
static const float LINK_BW_KHZ = 125.0;
#ifndef LINK_SF
#define LINK_SF 7
#endif
static const uint8_t LINK_CR = 7;
static const uint8_t LINK_SYNC = 0x12;
static const int8_t LINK_DBM = 22;
static const uint16_t LINK_PREAMBLE = 12;
static const float LINK_TCXO_V = 1.6;

static const int PIN_LORA_SCK = 9, PIN_LORA_MISO = 11, PIN_LORA_MOSI = 10;
static const int PIN_LORA_NSS = 8, PIN_LORA_DIO1 = 14, PIN_LORA_RST = 12, PIN_LORA_BUSY = 13;

static const uint8_t FRAME_DATA = 1, FRAME_ACK = 2;
static const uint8_t ACK_STORED = 1, ACK_FAILED = 0;
static const size_t LINK_TAG_BYTES = 8;
static const size_t DATA_HEADER_BYTES = 12;
static const uint8_t MAX_RECORDS = 6;

static const uint16_t NO_U16 = 0xFFFF;
static const int16_t NO_I16 = -32768;

struct __attribute__((packed)) LinkRecord {
  uint32_t bootId;     // 0 marks padding left by an interrupted write.
  uint32_t seq;
  uint32_t timestamp;  // 0 until the clock is set.
  uint16_t millivolts;
  int16_t currentX10;  // mA
  int16_t tempX100;    // C
  uint16_t humidityX10;
  int16_t tempBmpX100; // C
  uint16_t pressureX10;  // hPa
};

struct __attribute__((packed)) AckFrame {
  uint8_t a, b, type, status;
  uint32_t nonce;
  uint32_t epoch;
  uint32_t otaId;
  uint8_t tag[LINK_TAG_BYTES];
};

static const size_t MAX_DATA_BYTES = DATA_HEADER_BYTES + MAX_RECORDS * sizeof(LinkRecord) + LINK_TAG_BYTES;

static inline void linkTag(const char *key, const uint8_t *data, size_t len, uint8_t *tag) {
  uint8_t full[32];
  mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), (const uint8_t *)key, strlen(key),
                  data, len, full);
  memcpy(tag, full, LINK_TAG_BYTES);
}

static inline bool linkTagOk(const char *key, const uint8_t *data, size_t len, const uint8_t *tag) {
  uint8_t want[LINK_TAG_BYTES];
  linkTag(key, data, len, want);
  uint8_t diff = 0;
  for (size_t i = 0; i < LINK_TAG_BYTES; i++) {
    diff |= want[i] ^ tag[i];
  }
  return diff == 0;
}
