#pragma once

// Bosch BME280 (temp/humidity/pressure) over I2C — one of two chips
// env_sensor.cpp probes for. Minimal self-contained driver (no
// Adafruit_Sensor/BusIO) to keep flash footprint down; forced-mode reads
// every few seconds.
//
// Wire.begin() is owned by env_sensor.cpp, not this file — call
// bme280_try_init() only after Wire is already up.

bool  bme280_try_init(void);      // probes 0x76 then 0x77; true if a BME280 answered
void  bme280_tick(void);
bool  bme280_is_present(void);
float bme280_temp_c(void);        // last reading; 0 before first read
float bme280_humidity_pct(void);
float bme280_pressure_hpa(void);
