#include "fuel_gauge.h"
#include "board.h"
#include <Arduino.h>
#include <Wire.h>

// Standard BQ27220 (TI SBS-style) 16-bit word registers, little-endian.
// Register addresses are the chip's documented protocol, not vendored code.
#define REG_CURRENT 0x0C  // Current(), mA, signed
#define REG_SOC     0x2C  // StateOfCharge(), %

static bool read_word(uint8_t reg, uint16_t* out) {
    Wire.beginTransmission(BQ27220_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(BQ27220_ADDR, (uint8_t)2) != 2) return false;
    uint8_t lo = Wire.read();
    uint8_t hi = Wire.read();
    *out = ((uint16_t)hi << 8) | lo;
    return true;
}

bool fuel_gauge_init(void) {
    Wire.beginTransmission(BQ27220_ADDR);
    if (Wire.endTransmission() != 0) {
        Serial.println("BQ27220 not found");
        return false;
    }
    Serial.println("BQ27220 init OK");
    return true;
}

int fuel_gauge_read_pct(void) {
    uint16_t raw;
    if (!read_word(REG_SOC, &raw)) return -1;
    if (raw > 100) return -1;
    return (int)raw;
}

int fuel_gauge_read_current_ma(void) {
    uint16_t raw;
    if (!read_word(REG_CURRENT, &raw)) return 0;
    return (int16_t)raw;   // signed per datasheet
}
