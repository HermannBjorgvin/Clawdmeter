#include "io_expander.h"
#include "board.h"
#include <Arduino.h>
#include <Wire.h>

// TCA9554 register map
#define IOX_REG_OUTPUT   0x01
#define IOX_REG_CONFIG   0x03   // 1 = input, 0 = output

// EXIO1 (LCD reset) and EXIO7 (amp) are outputs; everything else is input.
#define IOX_CONFIG_MASK  ((uint8_t)~((1u << IOX_LCD_RESET) | (1u << IOX_PA_CTRL)))

// The first transaction after Wire.begin() fails on this board
// (ESP_ERR_INVALID_STATE), so each init write is retried and read back.
#define IOX_TRIES        3

static uint8_t output_state = 0x00;

static bool write_reg(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(IO_EXPANDER_ADDR);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

static bool read_reg(uint8_t reg, uint8_t& val) {
    Wire.beginTransmission(IO_EXPANDER_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(IO_EXPANDER_ADDR, (uint8_t)1) != 1) return false;
    val = Wire.read();
    return true;
}

static bool write_verified(uint8_t reg, uint8_t val) {
    for (int i = 0; i < IOX_TRIES; i++) {
        uint8_t got;
        if (write_reg(reg, val) && read_reg(reg, got) && got == val) return true;
        delay(5);
    }
    return false;
}

bool io_expander_init(void) {
    // Latch the outputs low (LCD in reset, amp off) before making them
    // outputs, so neither pin glitches high.
    output_state = 0x00;
    if (!write_verified(IOX_REG_OUTPUT, output_state) ||
        !write_verified(IOX_REG_CONFIG, IOX_CONFIG_MASK)) {
        Serial.println("TCA9554 init failed");
        return false;
    }
    delay(20);
    io_expander_set(IOX_LCD_RESET, true);   // release the LCD reset
    delay(120);                             // ST7796 needs 120 ms after reset
    Serial.println("TCA9554 init OK");
    return true;
}

void io_expander_set(uint8_t pin, bool high) {
    if (pin > 7) return;
    if (high) output_state |= (1u << pin);
    else      output_state &= ~(1u << pin);
    // Verified: a lost amp-off write would leave the NS4150B on until the
    // next ding.
    if (!write_verified(IOX_REG_OUTPUT, output_state))
        Serial.printf("TCA9554 EXIO%u write failed\n", pin);
}
