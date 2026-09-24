#pragma once

#include <stdint.h>

// TCA9554 / CH32V003-as-expander on the LCD-4. Must run before
// display_hal_init() so the panel power rails and backlight are up.

void io_expander_init(void);
void io_expander_set_backlight(bool on);
uint8_t io_expander_addr(void);  // 0 if probe failed
