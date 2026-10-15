#include "pm1.h"
#include <Arduino.h>
#include <Wire.h>

// Wire must already be up on the internal bus (board_init does this first).
// The PM1 speaks standard-mode I2C; keep transactions short — power_hal_tick
// runs these in the render loop.

bool pm1_write8(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(PM1_ADDR);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

int pm1_read8(uint8_t reg) {
    Wire.beginTransmission(PM1_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return -1;
    if (Wire.requestFrom((uint8_t)PM1_ADDR, (uint8_t)1) != 1) return -1;
    return Wire.read();
}

int pm1_read16(uint8_t reg_l) {
    Wire.beginTransmission(PM1_ADDR);
    Wire.write(reg_l);
    if (Wire.endTransmission(false) != 0) return -1;
    if (Wire.requestFrom((uint8_t)PM1_ADDR, (uint8_t)2) != 2) return -1;
    int lo = Wire.read();
    int hi = Wire.read();
    return (hi << 8) | lo;
}

bool pm1_bit_on(uint8_t reg, uint8_t mask) {
    int v = pm1_read8(reg);
    if (v < 0) return false;
    return pm1_write8(reg, (uint8_t)v | mask);
}

bool pm1_bit_off(uint8_t reg, uint8_t mask) {
    int v = pm1_read8(reg);
    if (v < 0) return false;
    return pm1_write8(reg, (uint8_t)v & ~mask);
}
