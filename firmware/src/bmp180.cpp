#include "bmp180.h"
#include <Arduino.h>
#include <Wire.h>

// Fixed-point compensation formulas are Bosch's reference implementation
// from the BMP180 datasheet (public reference code), oversampling=0 (single
// sample, ~4.5ms conversion) — plenty for a desk gadget, keeps the read
// path simple and non-blocking-ish (small fixed delay(), not a busy poll).

namespace {

constexpr uint8_t I2C_ADDR       = 0x77;   // BMP180's address is fixed
constexpr uint8_t REG_CHIP_ID    = 0xD0;
constexpr uint8_t REG_CTRL       = 0xF4;
constexpr uint8_t REG_RESULT     = 0xF6;
constexpr uint8_t CHIP_ID_BMP180 = 0x55;
constexpr uint8_t CMD_TEMP       = 0x2E;
constexpr uint8_t CMD_PRESS_OSS0 = 0x34;
constexpr uint32_t POLL_MS = 3000;

bool     present = false;
uint32_t last_poll_ms = 0;

int16_t  AC1, AC2, AC3, B1, B2, MB, MC, MD;
uint16_t AC4, AC5, AC6;

float last_temp_c = 0, last_press_hpa = 0;

bool write_reg(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(I2C_ADDR);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

bool read_regs(uint8_t reg, uint8_t* buf, size_t len) {
    Wire.beginTransmission(I2C_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((int)I2C_ADDR, (int)len) != (int)len) return false;
    for (size_t i = 0; i < len; i++) buf[i] = Wire.read();
    return true;
}

bool probe_and_read_calib() {
    Wire.beginTransmission(I2C_ADDR);
    Wire.write(REG_CHIP_ID);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((int)I2C_ADDR, 1) != 1) return false;
    if (Wire.read() != CHIP_ID_BMP180) return false;

    uint8_t c[22];   // 0xAA..0xBF: 11 big-endian int16 calibration words
    if (!read_regs(0xAA, c, 22)) return false;
    AC1 = (int16_t)(c[0]  << 8 | c[1]);
    AC2 = (int16_t)(c[2]  << 8 | c[3]);
    AC3 = (int16_t)(c[4]  << 8 | c[5]);
    AC4 = (uint16_t)(c[6]  << 8 | c[7]);
    AC5 = (uint16_t)(c[8]  << 8 | c[9]);
    AC6 = (uint16_t)(c[10] << 8 | c[11]);
    B1  = (int16_t)(c[12] << 8 | c[13]);
    B2  = (int16_t)(c[14] << 8 | c[15]);
    MB  = (int16_t)(c[16] << 8 | c[17]);
    MC  = (int16_t)(c[18] << 8 | c[19]);
    MD  = (int16_t)(c[20] << 8 | c[21]);
    return true;
}

bool trigger_and_read() {
    if (!write_reg(REG_CTRL, CMD_TEMP)) return false;
    delay(5);   // datasheet: 4.5ms max for temperature conversion
    uint8_t tb[2];
    if (!read_regs(REG_RESULT, tb, 2)) return false;
    int32_t UT = ((int32_t)tb[0] << 8) | tb[1];

    if (!write_reg(REG_CTRL, CMD_PRESS_OSS0)) return false;
    delay(5);   // datasheet: 4.5ms max at oss=0
    uint8_t pb[3];
    if (!read_regs(REG_RESULT, pb, 3)) return false;
    int32_t UP = (((int32_t)pb[0] << 16) | ((int32_t)pb[1] << 8) | pb[2]) >> 8;   // oss=0

    // -- temperature (0.1 DegC steps) --
    int32_t X1 = ((UT - (int32_t)AC6) * (int32_t)AC5) >> 15;
    int32_t X2 = ((int32_t)MC << 11) / (X1 + MD);
    int32_t B5 = X1 + X2;
    int32_t T = (B5 + 8) >> 4;
    last_temp_c = T / 10.0f;

    // -- pressure (Pa, oss=0) --
    int32_t B6 = B5 - 4000;
    X1 = ((int32_t)B2 * ((B6 * B6) >> 12)) >> 11;
    X2 = ((int32_t)AC2 * B6) >> 11;
    int32_t X3 = X1 + X2;
    int32_t B3 = (((int32_t)AC1 * 4 + X3) + 2) >> 2;
    X1 = ((int32_t)AC3 * B6) >> 13;
    X2 = ((int32_t)B1 * ((B6 * B6) >> 12)) >> 16;
    X3 = ((X1 + X2) + 2) >> 2;
    uint32_t B4 = ((uint32_t)AC4 * (uint32_t)(X3 + 32768)) >> 15;
    uint32_t B7 = ((uint32_t)UP - (uint32_t)B3) * 50000UL;
    int32_t p;
    if (B7 < 0x80000000UL) p = (int32_t)((B7 << 1) / B4);
    else                   p = (int32_t)((B7 / B4) << 1);
    X1 = (p >> 8) * (p >> 8);
    X1 = (X1 * 3038) >> 16;
    X2 = (-7357 * p) >> 16;
    p = p + ((X1 + X2 + 3791) >> 4);
    last_press_hpa = p / 100.0f;

    return true;
}

}  // namespace

bool bmp180_try_init(void) {
    present = probe_and_read_calib();
    if (present) Serial.println("BMP180 found at 0x77");
    return present;
}

void bmp180_tick(void) {
    if (!present) return;
    uint32_t now = millis();
    if (last_poll_ms != 0 && now - last_poll_ms < POLL_MS) return;
    last_poll_ms = now;
    if (!trigger_and_read()) {
        present = false;
        Serial.println("BMP180 read failed, dropping sensor");
    }
}

bool  bmp180_is_present(void)   { return present; }
float bmp180_temp_c(void)       { return last_temp_c; }
float bmp180_pressure_hpa(void) { return last_press_hpa; }
