#include "../../hal/touch_hal.h"

// No touch controller on the M5StickS3. The HAL permanently reports
// "released" — LVGL's pointer indev simply never fires a click. The side
// button stands in for it: power.cpp synthesizes the PWR role from BtnB
// (BoardCaps.has_pwr_button), which the director treats as you being here.

void touch_hal_init(void) {}

void touch_hal_read(uint16_t* x, uint16_t* y, bool* pressed) {
    *x = 0;
    *y = 0;
    *pressed = false;
}
