#include "../../hal/touch_hal.h"
#include "board.h"
#include <Arduino.h>
#include <Wire.h>

// TODO: replace the body with a driver for your controller. Two patterns:
//   1. A library (SensorLib's CSTxxx, TAMC_GT911, etc.) — add to lib_deps
//      in platformio.ini, mirror the AMOLED-2.16 port's touch.cpp.
//   2. A minimal vendored reader — preferred when the only available
//      library is GPL-licensed (see boards/waveshare_amoled_18/touch.cpp).
//
// Whichever you pick, touch_hal_read() must complete in well under 5 ms
// (a single I2C burst is fine) so it doesn't drop frames.

static volatile bool     touch_data_ready = false;
static volatile bool     touch_pressed = false;
static volatile uint16_t touch_x = 0;
static volatile uint16_t touch_y = 0;

static void IRAM_ATTR touch_isr(void) {
    touch_data_ready = true;
}

void touch_hal_init(void) {
    // TODO: initialize your controller over I2C; configure to active scanning.
    pinMode(TP_INT, INPUT_PULLUP);
    attachInterrupt(TP_INT, touch_isr, FALLING);
}

void touch_hal_read(uint16_t* x, uint16_t* y, bool* pressed) {
    if (touch_data_ready) {
        touch_data_ready = false;
        // TODO: read coords from your controller into touch_x, touch_y,
        // touch_pressed.
    }
    *x = touch_x;
    *y = touch_y;
    *pressed = touch_pressed;
}
