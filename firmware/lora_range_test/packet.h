#pragma once
#include <stdint.h>

struct __attribute__((packed)) Packet {
  uint8_t a, b, type;
  uint16_t seq;
  int16_t rssi;   // Signal the sender measured on the packet it answers.
  int8_t snrX4;
};
