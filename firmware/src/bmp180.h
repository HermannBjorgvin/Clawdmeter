#pragma once

// Bosch BMP180 (temp/pressure, no humidity) over I2C — one of two chips
// env_sensor.cpp probes for. Fixed address 0x77. Minimal self-contained
// driver (no Adafruit_Sensor/BusIO) to keep flash footprint down; forced
// reads every few seconds.
//
// Wire.begin() is owned by env_sensor.cpp, not this file — call
// bmp180_try_init() only after Wire is already up.

bool  bmp180_try_init(void);      // true if a BMP180 answered at 0x77
void  bmp180_tick(void);
bool  bmp180_is_present(void);    // false until a sensor answers on the bus
float bmp180_temp_c(void);        // last reading; 0 before first read
float bmp180_pressure_hpa(void);
