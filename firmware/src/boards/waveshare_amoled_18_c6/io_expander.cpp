#include "io_expander.h"
#include "board.h"
#include <Arduino.h>
#include <Wire.h>

// TCA9554/PCA9554 register map
#define IOX_REG_INPUT    0x00
#define IOX_REG_OUTPUT   0x01
#define IOX_REG_POLARITY 0x02
#define IOX_REG_CONFIG   0x03   // 1 = input, 0 = output

// P4 (display power), P5 (touch power), P7 (amp enable) are outputs; the rest
// stay inputs. Config bit = 1 means input, so the output mask is the inverse
// of bits 4, 5, 7: ~0b10110000 = 0x4F.
#define IOX_CONFIG_MASK    0x4F
// Display + touch powered on, amp left off (this firmware has no audio path,
// and enabling an idle class-D amp just adds hiss). Bits 4 | 5 = 0x30.
#define IOX_OUTPUT_DEFAULT 0x30

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
        Serial.println("TCA9554 init failed (config)");
        return false;
    }
    // Documented power-on sequence: display + touch power LOW, hold 200 ms,
    // then HIGH. The hold lets the panel rails fully discharge so the SH8601
    // sees a clean cold start (its only "reset" on this board).
    output_state = 0x00;
    write_reg(IOX_REG_OUTPUT, output_state);
    delay(200);
    output_state = IOX_OUTPUT_DEFAULT;
    write_reg(IOX_REG_OUTPUT, output_state);
    delay(120);   // let display + touch rails stabilize before probing
    Serial.println("TCA9554 init OK (display + touch powered)");
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
