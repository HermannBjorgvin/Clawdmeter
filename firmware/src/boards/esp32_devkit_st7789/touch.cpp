#include "../../hal/touch_hal.h"

// No touch controller on this build — a bare ST7789V2 module is display-only.
// Reporting "never pressed" is enough: main.cpp still registers the LVGL
// input device, it simply never fires, and every screen change goes through
// the PWR-role button instead of the tap-to-toggle gesture.
//
// If you later add a CST816 / FT6236 breakout, copy
// boards/waveshare_lcd_154/touch.cpp — it is a self-contained I2C reader —
// and add Wire.begin(sda, scl) to board_init.cpp.

void touch_hal_init(void) {}

void touch_hal_read(uint16_t* x, uint16_t* y, bool* pressed) {
    *x = 0;
    *y = 0;
    *pressed = false;
}
