#include "../../hal/touch_hal.h"

// No touch controller on the M5StickS3. The HAL permanently reports
// "released" — LVGL's pointer indev simply never fires a click. The tap
// gesture's job (splash/usage toggle) moves to the side button: power.cpp
// synthesizes the PWR role from BtnB and main.cpp routes its short press
// to ui_toggle_splash() via PWR_BTN_TOGGLES_SCREENS.

void touch_hal_init(void) {}

void touch_hal_read(uint16_t* x, uint16_t* y, bool* pressed) {
    *x = 0;
    *y = 0;
    *pressed = false;
}
