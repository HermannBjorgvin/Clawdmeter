#include "../../hal/touch_hal.h"

// No touch controller on this board. Reporting "never pressed" is enough:
// main.cpp still registers the LVGL input device, it simply never fires, and
// screen changes go through the PWR-role button instead of the tap-to-toggle
// gesture (BOARD_HAS_TOUCH 0 → caps.has_touch false is what routes that).
//
// The T-Display-S3 sibling ships a CST816 variant; if you wire one up here,
// copy boards/waveshare_lcd_154/touch.cpp — it is a self-contained I2C reader
// for that exact controller — add Wire.begin(sda, scl) to board_init.cpp, and
// flip BOARD_HAS_TOUCH to 1.

void touch_hal_init(void) {}

void touch_hal_read(uint16_t* x, uint16_t* y, bool* pressed) {
    *x = 0;
    *y = 0;
    *pressed = false;
}
