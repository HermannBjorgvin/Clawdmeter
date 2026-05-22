#include "io_expander.h"
#include "board.h"
#include <Arduino.h>
#include <Wire.h>

// XCA9554/PCA9554 register map
#define IOX_REG_INPUT    0x00
#define IOX_REG_OUTPUT   0x01
#define IOX_REG_POLARITY 0x02
#define IOX_REG_CONFIG   0x03   // 1 = input, 0 = output

// EXIO0..2 are outputs (reset lines + audio amp). Everything else is input.
// Bit layout: 0bIIIIIOOO = 0xF8
#define IOX_CONFIG_MASK    0xF8
// All three outputs HIGH = resets released, amp enabled.
#define IOX_OUTPUT_DEFAULT 0x07

static uint8_t output_state = 0x00;

static bool write_reg(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(XCA9554_ADDR);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

static bool read_reg(uint8_t reg, uint8_t& val) {
    Wire.beginTransmission(XCA9554_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(XCA9554_ADDR, (uint8_t)1) != 1) return false;
    val = Wire.read();
    return true;
}

bool io_expander_init(void) {
    if (!write_reg(IOX_REG_CONFIG, IOX_CONFIG_MASK)) {
        Serial.println("XCA9554 init failed (config)");
        return false;
    }
    // Hold display + touch in reset.
    output_state = 0x00;
    write_reg(IOX_REG_OUTPUT, output_state);
    delay(20);
    // Release resets and enable audio amp output line.
    output_state = IOX_OUTPUT_DEFAULT;
    write_reg(IOX_REG_OUTPUT, output_state);
    delay(20);
    Serial.println("XCA9554 init OK");
    return true;
}

void io_expander_set(uint8_t pin, bool high) {
    if (pin > 7) return;
    if (high) output_state |= (1u << pin);
    else      output_state &= ~(1u << pin);
    write_reg(IOX_REG_OUTPUT, output_state);
}

bool io_expander_get(uint8_t pin) {
    if (pin > 7) return false;
    uint8_t v = 0;
    if (!read_reg(IOX_REG_INPUT, v)) return false;
    return (v & (1u << pin)) != 0;
}
