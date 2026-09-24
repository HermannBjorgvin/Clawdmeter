#pragma once
#include <stdint.h>

// TCA9554 / PCA9554-compatible 8-bit I2C IO expander.
// Board-private to the C6 AMOLED-1.8 port; not exposed in hal/.
//
// On this board the expander gates display power (P4) and touch power (P5),
// so it MUST be brought up (and the power-on sequence run) BEFORE the display
// or touch are initialized — otherwise both panels stay unpowered and fail to
// probe. board_init() calls io_expander_init() before display_hal_init().

bool io_expander_init(void);
void io_expander_set(uint8_t pin, bool high);
bool io_expander_get(uint8_t pin);
