#pragma once
#include <stdint.h>

// TCA9554 8-bit I2C IO expander at 0x20. Board-private to the LCD-3.5-C port;
// not exposed in hal/.
//
// Must be initialized BEFORE the display: EXIO1 is the ST7796 reset.
// EXIO7 is the NS4150B speaker amp's CTRL (high = on).

bool io_expander_init(void);
void io_expander_set(uint8_t pin, bool high);
