# Truck Monitor Build Log

## Phase 2: Bench test (2026-10-31)

Tag: `v0.1-bench`

### Hardware
- Heltec WiFi LoRa32 V3 (MAC b0:81:84:9b:93:5c), reflashed from Meshtastic
- INA219 at 0x40, AHT2x at 0x38, BMP280 (HW-611) at 0x76
- Sensors on a separate I2C bus: SDA GPIO41, SCL GPIO42, powered from 3V3
- Onboard OLED: SDA GPIO17, SCL GPIO18, RST GPIO21, rail switched by GPIO36

### Test setup
- Supply + to INA219 VIN+, VIN- to a 996 ohm resistor, resistor to supply -
- Supply - common with Heltec GND; Heltec powered from USB
- Voltage measured at VIN- to GND, where the INA219 reads bus voltage
- Reference current calculated as meter voltage / 996 ohm, because the supply
  display only resolves 1 mA

### Results

| Check | Reference | Sensor | Difference | Limit | Result |
|---|---|---|---|---|---|
| Voltage, 11V | 11.18 V | 11.195 V | +0.015 V | +/-0.1 V | Pass |
| Voltage, 12.6V | 12.60 V | 12.621 V | +0.021 V | +/-0.1 V | Pass |
| Voltage, 14.4V | 14.40 V | 14.423 V | +0.023 V | +/-0.1 V | Pass |
| Current, 11V | 11.22 mA | 11.24 mA | +0.2% | +/-5% | Pass |
| Current, 12.6V | 12.65 mA | 12.50 mA | -1.2% | +/-5% | Pass |
| Current, 14.4V | 14.46 mA | 14.38 mA | -0.6% | +/-5% | Pass |
| Temp, AHT2x | 22.28 C (72.1 F) | 21.97 C | -0.31 C | +/-1 C | Pass |
| Temp, second point | not run | | | +/-1 C | Waived |
| Humidity | house sensor | about 65% RH | agrees, per Ray | sanity check | Pass |
| Sleep/wake | 5 in a row | 6 in a row | 376 ms to record | under 5 s | Pass |

Each voltage and current point was repeatable across at least 6 samples.

The second temp point was waived by Ray: the truck install changes the thermal
conditions, so bench temperature adjustments would not carry over.

The 12.60 V and 14.40 V meter values were reported as matching the supply.

The humidity check was a sanity comparison against a house sensor, with no
reference number recorded.

### Sleep/wake
Final firmware, timer deep sleep at 20 s with the OLED on for 3 s per wake:
boot counts 2 through 7 consecutive, all timer wakes, 23.4 s apart, and all
sensors and the OLED initialized on every wake. "pro cpu reset by JTAG" appears
in the ROM boot log on these wakes and did not affect operation.

### Issues found
- BMP280 reads about 1 C above the AHT2x, and 1.10 C above the reference at
  room temp. It is a cross-check only, so it stays uncorrected.
- During a cold test the BMP280 gave a false 182 C reading, then failed to
  initialize. It recovered after being reseated and dried. The cause was a loose
  contact or moisture, not confirmed which. It will need solid connections and
  protection from condensation in the truck.
- A bench short on the DPS3010U tripped OCP, so no current limit was set by
  shorting. The load resistor limited the current during these tests.

### Deferred
- Current was only tested up to about 14 mA, far below what the fuse tap may
  carry. Retest near the real load before relying on the +/-5% result there.
- Sleep current has not been measured.
- Set `ENABLE_OLED` to 0 in `truck_node.ino` before installing in the truck.
- `timestamp` is not yet produced: records carry boot count and wake time until
  WiFi/NTP arrives in Phase 3.
