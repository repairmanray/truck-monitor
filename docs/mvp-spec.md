# Truck Monitor — MVP Spec (Phase 1)

## Scope
v1 = Heltec WiFi LoRa32 V3 reads battery voltage/current (INA219) and 
temperature/humidity (AHT21), syncs to CT100 via WiFi when parked, 
buffers to onboard flash when not.

Out of scope for v1: LoRa, OBD2, Pi3, GPS, accelerometer, and 
whole-truck current sensing (INA226). Each gets its own phase.

## Hardware
- Board: Heltec WiFi LoRa32 V3
- Voltage/current: INA219, I2C (address 0x40 default), inline on 
  fuse 12 (I.O.D.) tap
- Temp/humidity: AHT21, I2C (address 0x38 default), mounted inside 
  the enclosure alongside the Heltec
- Power tap: fuse-tap adapter into fuse 12 (I.O.D.), buck converter 
  to 5V

## Data schema
One log record per sample:

{
  timestamp: unix epoch,
  voltage: float (V),
  current: float (mA),
  temp: float (degrees C),
  humidity: float (%RH),
  synced: bool
}

## Success criteria
- Voltage reading within +/-0.1V of a multimeter
- Current reading within +/-5% of a known test load
- Temp reading within +/-1C of a reference thermometer
- Humidity reading within +/-5% RH of a reference hygrometer 
  (or reasonable sanity check against a known humid/dry source)
- All readings repeatable across 5 separate samples
