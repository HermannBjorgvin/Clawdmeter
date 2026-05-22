#pragma once
#include <stdint.h>

// XCA9554 / PCA9554-compatible 8-bit I2C IO expander.
// Board-private to the AMOLED-1.8 port; not exposed in hal/.
//
// Must be initialized BEFORE the display or touch — skipping the reset
// release leaves the SH8601 and FT3168 in reset and they fail to probe.

bool io_expander_init(void);
void io_expander_set(uint8_t pin, bool high);
bool io_expander_get(uint8_t pin);
