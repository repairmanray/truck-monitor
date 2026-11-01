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

## Phase 3: WiFi sync and flash fallback (2026-10-31)

Tag: `v0.2-sync`

### What was built
- `server/collector.py` on CT100 (`truck-log.service`, port 8101, SQLite at
  `~/truck-log/truck.db`). Needs the `X-Truck-Token` header; the token file
  stays on CT100 and in the node's NVS only.
- Node writes each record to LittleFS first, then uploads the queue, and removes
  records only after an HTTP 200. Records are unique on (device, boot_epoch,
  seq), so a resend can't create duplicates.
- Fallback storage is onboard flash, not SD: the Heltec has no SD slot, and
  `docs/mvp-spec.md` already called for onboard flash.
- `tools/set_net.py` loads WiFi and collector settings over USB without printing
  them.

### Results

| Check | Requirement | Result |
|---|---|---|
| Synced | Within 5 s, 5 in a row | Pass: seq 2 to 6 took 5, 3, 4, 4, 4 s |
| WiFi down | 5 cycles, zero loss | Pass: 6 cycles, each stored, queue rose 2 to 7 |
| Backlog | Uploads when WiFi returns | Pass: 8 records sent, queue 0, 0 of 6 outage records missing |
| Collector | Rejects bad input | Pass: bad token 403, bad line rejected, resend ignored |

WiFi was taken down by pointing the node at a nonexistent SSID. Latency is
measured from the record's whole-second timestamp to the collector's
received time, so each figure is accurate to about 1 s.

### Notes
- The 5 s sync limit has little margin: the worst case above was at 5 s. The
  first sync after power-up took 6.3 s because it also sets the clock, and one
  later wake took 5.3 s in the node's own timer, which includes WiFi shutdown.
- Records taken before the first NTP sync have a null timestamp. CT100's
  received time is the only time for those.
- Opening the USB serial port resets the board, which starts a new boot_epoch.

### Deferred
- No handling for a full queue. The flash partition is about 1.5 MB, and a full
  disk would lose new records. Size this and set a policy before long parked
  stretches away from WiFi.
- A failed WiFi attempt costs about 6 s awake each wake, which will matter for
  battery once the wake interval is set. Consider backing off.
- The 20 s wake interval is a bench value.
- Reading in the truck needs WiFi coverage where it parks. Check this in Phase 4.
