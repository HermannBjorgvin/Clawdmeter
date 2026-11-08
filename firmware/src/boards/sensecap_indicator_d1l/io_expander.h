#pragma once
#include <stdint.h>

// PCA9535 16-bit I2C IO expander — minimal driver, port 0 only.
// Must be initialized before display_hal_init() so the LCD SPI CS and
// touch RST lines are configured before gfx->begin() runs.

bool io_expander_init(void);
void io_expander_set(uint8_t pin, bool high);
bool io_expander_get(uint8_t pin);
