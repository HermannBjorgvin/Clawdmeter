#include "../../hal/touch_hal.h"

// No touchscreen on the Xingzhi Cube 1.83 — provide empty stubs so the
// shared LVGL touch driver hook compiles and just never reports a press.

void touch_hal_init(void) {}

void touch_hal_read(uint16_t* x, uint16_t* y, bool* pressed) {
    *x = 0;
    *y = 0;
    *pressed = false;
}
