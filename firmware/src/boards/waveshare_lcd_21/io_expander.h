#pragma once

#include <stdint.h>

// TCA9554 on the LCD-2.1. It holds the LCD and touch reset lines and the
// ST7701S SPI chip select, so it must run before display_hal_init().

// Clocks a stuck I2C bus free; call before Wire.begin(). True when SDA and
// SCL both read high afterwards.
bool io_expander_recover_bus(void);
void io_expander_init(void);
// Returns false when the I2C write failed.
bool io_expander_set(uint8_t bit, bool high);
