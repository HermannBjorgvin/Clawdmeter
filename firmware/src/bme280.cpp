#include "bme280.h"
#include <Arduino.h>
#include <Wire.h>

// Fixed-point compensation formulas are Bosch's reference implementation
// from the BME280 datasheet (public reference code) — kept in integer/int64
// math (no double) so no soft-float lib gets pulled into the tight flash
// budget beyond what LVGL already links.

namespace {

constexpr uint8_t ADDR_CANDIDATES[2] = {0x76, 0x77};
constexpr uint8_t REG_CHIP_ID   = 0xD0;
constexpr uint8_t REG_CTRL_HUM  = 0xF2;
constexpr uint8_t REG_STATUS    = 0xF3;
constexpr uint8_t REG_CTRL_MEAS = 0xF4;
constexpr uint8_t REG_CONFIG    = 0xF5;
constexpr uint8_t REG_PRESS_MSB = 0xF7;
constexpr uint8_t CHIP_ID_BME280 = 0x60;
constexpr uint32_t POLL_MS = 3000;

uint8_t  i2c_addr = 0;
bool     present = false;
uint32_t last_poll_ms = 0;

uint16_t dig_T1; int16_t dig_T2, dig_T3;
uint16_t dig_P1; int16_t dig_P2, dig_P3, dig_P4, dig_P5, dig_P6, dig_P7, dig_P8, dig_P9;
uint8_t  dig_H1, dig_H3; int16_t dig_H2, dig_H4, dig_H5; int8_t dig_H6;
int32_t  t_fine = 0;

float last_temp_c = 0, last_hum_pct = 0, last_press_hpa = 0;

bool write_reg(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(i2c_addr);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

bool read_regs(uint8_t reg, uint8_t* buf, size_t len) {
    Wire.beginTransmission(i2c_addr);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((int)i2c_addr, (int)len) != (int)len) return false;
    for (size_t i = 0; i < len; i++) buf[i] = Wire.read();
    return true;
}

bool probe_and_read_calib() {
    for (uint8_t addr : ADDR_CANDIDATES) {
        i2c_addr = addr;
        Wire.beginTransmission(addr);
        Wire.write(REG_CHIP_ID);
        if (Wire.endTransmission(false) != 0) continue;
        if (Wire.requestFrom((int)addr, 1) != 1) continue;
        if (Wire.read() != CHIP_ID_BME280) continue;

        uint8_t c1[26];   // 0x88..0xA1: dig_T1-3, dig_P1-9, (reserved), dig_H1
        if (!read_regs(0x88, c1, 26)) continue;
        dig_T1 = (uint16_t)(c1[1] << 8 | c1[0]);
        dig_T2 = (int16_t)(c1[3] << 8 | c1[2]);
        dig_T3 = (int16_t)(c1[5] << 8 | c1[4]);
        dig_P1 = (uint16_t)(c1[7] << 8 | c1[6]);
        dig_P2 = (int16_t)(c1[9] << 8 | c1[8]);
        dig_P3 = (int16_t)(c1[11] << 8 | c1[10]);
        dig_P4 = (int16_t)(c1[13] << 8 | c1[12]);
        dig_P5 = (int16_t)(c1[15] << 8 | c1[14]);
        dig_P6 = (int16_t)(c1[17] << 8 | c1[16]);
        dig_P7 = (int16_t)(c1[19] << 8 | c1[18]);
        dig_P8 = (int16_t)(c1[21] << 8 | c1[20]);
        dig_P9 = (int16_t)(c1[23] << 8 | c1[22]);
        dig_H1 = c1[25];

        uint8_t c2[7];    // 0xE1..0xE7: dig_H2-6
        if (!read_regs(0xE1, c2, 7)) continue;
        dig_H2 = (int16_t)(c2[1] << 8 | c2[0]);
        dig_H3 = c2[2];
        dig_H4 = (int16_t)(((int8_t)c2[3] << 4) | (c2[4] & 0x0F));
        dig_H5 = (int16_t)(((int8_t)c2[5] << 4) | (c2[4] >> 4));
        dig_H6 = (int8_t)c2[6];

        // ctrl_hum only takes effect after the next ctrl_meas write (below,
        // on the first forced-mode trigger).
        write_reg(REG_CTRL_HUM, 0x01);   // humidity oversampling x1
        write_reg(REG_CONFIG, 0x00);     // no IIR filter (forced mode: standby n/a)
        return true;
    }
    return false;
}

bool trigger_and_read() {
    // temp x1, press x1, forced mode (0b01)
    if (!write_reg(REG_CTRL_MEAS, 0x25)) return false;

    uint32_t start = millis();
    uint8_t status;
    do {
        if (millis() - start > 50) return false;   // conversion is ~9ms typ.
        if (!read_regs(REG_STATUS, &status, 1)) return false;
    } while (status & 0x08);

    uint8_t raw[8];
    if (!read_regs(REG_PRESS_MSB, raw, 8)) return false;

    int32_t adc_P = ((int32_t)raw[0] << 12) | ((int32_t)raw[1] << 4) | (raw[2] >> 4);
    int32_t adc_T = ((int32_t)raw[3] << 12) | ((int32_t)raw[4] << 4) | (raw[5] >> 4);
    int32_t adc_H = ((int32_t)raw[6] << 8)  | raw[7];

    // -- temperature (0.01 DegC steps) --
    int32_t var1t, var2t;
    var1t = ((((adc_T >> 3) - ((int32_t)dig_T1 << 1))) * ((int32_t)dig_T2)) >> 11;
    var2t = (((((adc_T >> 4) - ((int32_t)dig_T1)) * ((adc_T >> 4) - ((int32_t)dig_T1))) >> 12) *
             ((int32_t)dig_T3)) >> 14;
    t_fine = var1t + var2t;
    int32_t T = (t_fine * 5 + 128) >> 8;
    last_temp_c = T / 100.0f;

    // -- pressure (Q24.8 Pa) --
    int64_t var1p, var2p, p;
    var1p = ((int64_t)t_fine) - 128000;
    var2p = var1p * var1p * (int64_t)dig_P6;
    var2p = var2p + ((var1p * (int64_t)dig_P5) << 17);
    var2p = var2p + (((int64_t)dig_P4) << 35);
    var1p = ((var1p * var1p * (int64_t)dig_P3) >> 8) + ((var1p * (int64_t)dig_P2) << 12);
    var1p = (((((int64_t)1) << 47) + var1p)) * ((int64_t)dig_P1) >> 33;
    if (var1p == 0) {
        last_press_hpa = 0;
    } else {
        p = 1048576 - adc_P;
        p = (((p << 31) - var2p) * 3125) / var1p;
        var1p = (((int64_t)dig_P9) * (p >> 13) * (p >> 13)) >> 25;
        var2p = (((int64_t)dig_P8) * p) >> 19;
        p = ((p + var1p + var2p) >> 8) + (((int64_t)dig_P7) << 4);
        last_press_hpa = (uint32_t)p / 256.0f / 100.0f;
    }

    // -- humidity (Q22.10 %RH) --
    int32_t v_x1;
    v_x1 = (t_fine - ((int32_t)76800));
    v_x1 = (((((adc_H << 14) - (((int32_t)dig_H4) << 20) - (((int32_t)dig_H5) * v_x1)) +
             ((int32_t)16384)) >> 15) * (((((((v_x1 * ((int32_t)dig_H6)) >> 10) *
             (((v_x1 * ((int32_t)dig_H3)) >> 11) + ((int32_t)32768))) >> 10) + ((int32_t)2097152)) *
             ((int32_t)dig_H2) + 8192) >> 14));
    v_x1 = (v_x1 - (((((v_x1 >> 15) * (v_x1 >> 15)) >> 7) * ((int32_t)dig_H1)) >> 4));
    v_x1 = v_x1 < 0 ? 0 : v_x1;
    v_x1 = v_x1 > 419430400 ? 419430400 : v_x1;
    last_hum_pct = (uint32_t)(v_x1 >> 12) / 1024.0f;

    return true;
}

}  // namespace

bool bme280_try_init(void) {
    present = probe_and_read_calib();
    if (present) Serial.printf("BME280 found at 0x%02X\n", i2c_addr);
    return present;
}

void bme280_tick(void) {
    if (!present) return;
    uint32_t now = millis();
    if (last_poll_ms != 0 && now - last_poll_ms < POLL_MS) return;
    last_poll_ms = now;
    if (!trigger_and_read()) {
        present = false;
        Serial.println("BME280 read failed, dropping sensor");
    }
}

bool  bme280_is_present(void)   { return present; }
float bme280_temp_c(void)       { return last_temp_c; }
float bme280_humidity_pct(void) { return last_hum_pct; }
float bme280_pressure_hpa(void) { return last_press_hpa; }
