// Truck Monitor Phase 2: I2C bus scan for the Heltec WiFi LoRa32 V3.
// Sensors sit on GPIO41/42, separate from the OLED bus on GPIO17/18.
// Expected: 0x38 AHT2x, 0x40 INA219, 0x76 or 0x77 BMP280.

#include <Wire.h>

static const int PIN_SDA = 41;
static const int PIN_SCL = 42;

static const char *deviceName(uint8_t addr) {
  switch (addr) {
    case 0x38: return "AHT2x";
    case 0x40: return "INA219";
    case 0x76:
    case 0x77: return "BMP280";
    default:   return "unknown";
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Wire.begin(PIN_SDA, PIN_SCL, 100000);
  Serial.println("Truck Monitor I2C scan on SDA=41 SCL=42");
}

void loop() {
  int found = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  0x%02X %s\n", addr, deviceName(addr));
      found++;
    }
  }
  Serial.printf("scan done: %d device(s)\n\n", found);
  delay(3000);
}
