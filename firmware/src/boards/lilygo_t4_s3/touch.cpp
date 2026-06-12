#include "../../hal/touch_hal.h"
#include "board.h"
#include <Arduino.h>

// CHSC5816 touch controller — initialised inside amoled.beginAMOLED_241().

void touch_hal_init(void) {
    // Touch is already up from board_init(); nothing extra needed.
    Serial.println("Touch HAL init OK (CHSC5816 via LilyGo)");
}

void touch_hal_read(uint16_t* x, uint16_t* y, bool* pressed) {
    int16_t tx = 0, ty = 0;
    uint8_t n = amoled.getPoint(&tx, &ty, 1);
    if (n > 0) {
        *x       = (uint16_t)tx;
        *y       = (uint16_t)ty;
        *pressed = true;
    } else {
        *pressed = false;
    }
}
