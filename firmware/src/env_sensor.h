#pragma once

// Dispatcher for the board's environmental sensor: probes for a BME280
// first (temp/humidity/pressure), falls back to a BMP180 (temp/pressure,
// no humidity) if that fails. Whichever one is wired up "just works" —
// main.cpp/ui.cpp only ever talk to this file, never bme280.*/bmp180.*
// directly. Owns the one Wire.begin(21, 22) call for the bus.

void  env_sensor_init(void);
void  env_sensor_tick(void);
bool  env_sensor_is_present(void);
float env_sensor_temp_c(void);
float env_sensor_pressure_hpa(void);
bool  env_sensor_has_humidity(void);   // true only when the active chip is a BME280
float env_sensor_humidity_pct(void);   // valid only when the above is true

// Debug: raw I2C bus scan (0x03-0x77), prints ACKing addresses over Serial.
// Chip-agnostic — tells wiring/power problems (nothing ACKs) apart from an
// unrecognized chip (something ACKs but isn't a BME280/BMP180 chip ID).
void env_sensor_scan_bus(void);

// Debug: detaches the pins from I2C, reads GPIO21/22 as plain inputs with
// internal pull-up enabled, then re-attaches I2C. A line that reads LOW
// despite the internal pull-up is being held down by something external
// (bad joint, short, or a damaged pin) — independent of any sensor chip.
void env_sensor_gpio_test(void);
