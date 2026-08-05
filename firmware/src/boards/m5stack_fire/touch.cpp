#include "../../hal/touch_hal.h"

// The M5Stack FIRE has no touch panel — navigation is the three front buttons
// (see input.cpp for A/C and power.cpp for the middle button). The touch HAL
// is a permanent no-op: read always reports "not pressed", so LVGL's pointer
// indev stays idle and the shared my_touch_cb never registers a press.

void touch_hal_init(void) {}

void touch_hal_read(uint16_t* x, uint16_t* y, bool* pressed) {
    *x = 0;
    *y = 0;
    *pressed = false;
}
