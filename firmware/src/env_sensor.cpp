#include "env_sensor.h"
#include "bme280.h"
#include "bmp180.h"
#include <Arduino.h>
#include <Wire.h>

namespace {

enum Kind { KIND_NONE, KIND_BME280, KIND_BMP180 };
Kind active = KIND_NONE;

}  // namespace

void env_sensor_init(void) {
    Wire.begin(21, 22);
    Wire.setTimeout(50);

    if (bme280_try_init())      active = KIND_BME280;
    else if (bmp180_try_init()) active = KIND_BMP180;
    else                        active = KIND_NONE;

    if (active == KIND_NONE) Serial.println("env sensor: none found, skipping");
}

void env_sensor_tick(void) {
    if (active == KIND_BME280)      bme280_tick();
    else if (active == KIND_BMP180) bmp180_tick();
}

bool env_sensor_is_present(void) {
    if (active == KIND_BME280)      return bme280_is_present();
    if (active == KIND_BMP180)      return bmp180_is_present();
    return false;
}

float env_sensor_temp_c(void) {
    if (active == KIND_BME280)      return bme280_temp_c();
    if (active == KIND_BMP180)      return bmp180_temp_c();
    return 0;
}

float env_sensor_pressure_hpa(void) {
    if (active == KIND_BME280)      return bme280_pressure_hpa();
    if (active == KIND_BMP180)      return bmp180_pressure_hpa();
    return 0;
}

bool env_sensor_has_humidity(void) {
    return active == KIND_BME280;
}

float env_sensor_humidity_pct(void) {
    return active == KIND_BME280 ? bme280_humidity_pct() : 0;
}

void env_sensor_scan_bus(void) {
    Serial.println("i2c scan 0x03-0x77...");
    int found = 0;
    for (uint8_t addr = 0x03; addr <= 0x77; addr++) {
        Wire.beginTransmission(addr);
        uint8_t err = Wire.endTransmission();
        if (err == 0) {
            Serial.printf("  ACK at 0x%02X\n", addr);
            found++;
        }
    }
    if (!found) Serial.println("  nothing responded - check wiring/power, not the chip logic");
    else        Serial.printf("scan done, %d device(s)\n", found);
}

void env_sensor_gpio_test(void) {
    Wire.end();
    pinMode(21, INPUT_PULLUP);
    pinMode(22, INPUT_PULLUP);
    delay(5);
    int sda = digitalRead(21);
    int scl = digitalRead(22);
    Serial.printf("GPIO21 (SDA) with internal pull-up: %s\n", sda ? "HIGH (pin is fine)" : "LOW (held down externally!)");
    Serial.printf("GPIO22 (SCL) with internal pull-up: %s\n", scl ? "HIGH (pin is fine)" : "LOW (held down externally!)");
    Wire.begin(21, 22);
    Wire.setTimeout(50);
}
