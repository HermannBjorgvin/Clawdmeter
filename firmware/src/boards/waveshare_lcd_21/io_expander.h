#pragma once

#include <stdint.h>

// TCA9554 on the LCD-2.1. It holds the LCD and touch reset lines and the
// ST7701S SPI chip select, so it must run before display_hal_init().

void io_expander_init(void);
void io_expander_set(uint8_t bit, bool high);
