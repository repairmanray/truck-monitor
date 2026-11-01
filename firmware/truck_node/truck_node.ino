// Truck Monitor Phase 2: bench sensor node for the Heltec WiFi LoRa32 V3.
// Wakes, prints one JSON record over USB serial, then deep sleeps.
// No WiFi or logging yet. Sensors sit on GPIO41/42, separate from the
// OLED bus on GPIO17/18.

#include <Wire.h>
#include <esp_sleep.h>
#include <Adafruit_INA219.h>
#include <Adafruit_AHTX0.h>
#include <Adafruit_BMP280.h>
#include <Adafruit_SSD1306.h>
#include <driver/gpio.h>

// Bench diagnostics screen. Set to 0 for the truck to save battery.
#define ENABLE_OLED 1

static const int PIN_SDA = 41;
static const int PIN_SCL = 42;
static const uint8_t BMP280_ADDR = 0x76;
static const uint64_t SLEEP_SECONDS = 20;

static const int PIN_OLED_SDA = 17;
static const int PIN_OLED_SCL = 18;
static const int PIN_OLED_RST = 21;
static const gpio_num_t PIN_VEXT = GPIO_NUM_36;  // Low turns the OLED rail on.
static const uint8_t OLED_ADDR = 0x3C;
static const uint32_t OLED_HOLD_MS = 3000;

// Survives deep sleep, so consecutive values prove no wake was missed.
RTC_DATA_ATTR uint32_t bootCount = 0;

Adafruit_INA219 ina219;
Adafruit_AHTX0 aht;
Adafruit_BMP280 bmp(&Wire);

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
                     float tempC, float humidity, float tempBmp, float pressure) {
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(0, 0);
  oled.printf("TRUCK boot %lu\n", (unsigned long)bootCount);
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

// Prints a float, or null when the sensor is absent or returned NaN.
static void printField(const char *name, float value, int decimals, bool ok) {
  Serial.printf("\"%s\":", name);
  if (ok && !isnan(value)) {
    Serial.print(value, decimals);
  } else {
    Serial.print("null");
  }
  Serial.print(",");
}

void setup() {
  bootCount++;
  bool timerWake = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER;

  Serial.begin(115200);
  delay(100);
  Wire.begin(PIN_SDA, PIN_SCL, 100000);

#if ENABLE_OLED
  bool oledOk = oledBegin();
#else
  bool oledOk = false;
#endif

  bool inaOk = ina219.begin(&Wire);
  bool ahtOk = aht.begin(&Wire);
  bool bmpOk = bmp.begin(BMP280_ADDR);

  float busV = NAN, shuntMv = NAN, currentMa = NAN;
  float tempC = NAN, humidity = NAN, tempBmp = NAN, pressure = NAN;

  if (inaOk) {
    busV = ina219.getBusVoltage_V();
    shuntMv = ina219.getShuntVoltage_mV();
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

  // Uptime stands in for the spec's unix timestamp until WiFi/NTP in Phase 3.
  Serial.printf("{\"boot\":%lu,\"timer_wake\":%d,\"wake_ms\":%lu,\"oled\":%d,",
                (unsigned long)bootCount, timerWake, (unsigned long)millis(), oledOk);
  printField("voltage", busV, 3, inaOk);
  printField("shunt_mv", shuntMv, 2, inaOk);
  printField("current", currentMa, 1, inaOk);
  printField("temp", tempC, 2, ahtOk);
  printField("humidity", humidity, 1, ahtOk);
  printField("temp_bmp", tempBmp, 2, bmpOk);
  printField("pressure", pressure, 2, bmpOk);
  Serial.println("\"end\":1}");
  Serial.flush();

#if ENABLE_OLED
  if (oledOk) {
    oledShow(inaOk, ahtOk, bmpOk, busV, currentMa, tempC, humidity, tempBmp, pressure);
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
