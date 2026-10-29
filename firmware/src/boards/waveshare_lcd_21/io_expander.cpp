#include "io_expander.h"
#include "board.h"
#include <Arduino.h>
#include <Wire.h>

// TCA9554: 0x01 = output latch, 0x03 = config (1 = input). Power-on state is
// all inputs with the latch at 0xFF, so the latch is written before the
// config: resets released, CS deselected, buzzer off. EXIO5-7 carry the IMU
// and RTC interrupts and stay inputs (the Waveshare demo drives all eight).

#define IOX_REG_OUTPUT  0x01
#define IOX_REG_CONFIG  0x03
#define IOX_INPUTS      0x70   // EXIO5, EXIO6, EXIO7

static bool    expander_ok  = false;
static uint8_t output_latch = (uint8_t)~(1u << IOX_BIT_BUZZER);

static void iox_write(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(IO_EXPANDER_ADDR);
    Wire.write(reg);
    Wire.write(val);
    Wire.endTransmission();
}

void io_expander_init(void) {
    Wire.beginTransmission(IO_EXPANDER_ADDR);
    if (Wire.endTransmission() != 0) {
        Serial.println("LCD-2.1 IO expander not found (0x20)");
        return;
    }
    expander_ok = true;
    iox_write(IOX_REG_OUTPUT, output_latch);
    iox_write(IOX_REG_CONFIG, IOX_INPUTS);
    Serial.println("LCD-2.1 IO expander init OK");
}

void io_expander_set(uint8_t bit, bool high) {
    if (!expander_ok) return;
    if (high) output_latch |= (uint8_t)(1u << bit);
    else      output_latch &= (uint8_t)~(1u << bit);
    iox_write(IOX_REG_OUTPUT, output_latch);
}
