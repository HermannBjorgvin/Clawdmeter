#include "board.h"
#include <Arduino.h>

// Nothing to bring up before the display: no I2C devices, no IO expander, and
// the ST7789's reset line is a direct GPIO that Arduino_ST7789 pulses itself
// from gfx->begin(). Kept as an empty hook because main.cpp calls it
// unconditionally — add Wire.begin(sda, scl) here if you wire up touch.
extern "C" void board_init(void) {}
