#include "../../hal/touch_hal.h"
#include "board.h"
#include <Arduino.h>
#include <Wire.h>
#include <TouchDrvGoodix.hpp>

// GT911 via SensorLib — the same driver the LCD-4 port uses for this
// controller.
//
// Neither INT nor RST is wired on this board, so the I2C address is whatever
// the controller latched at power-on: probe 0x5D, fall back to 0x14.

static TouchDrvGT911 touch;

static bool     touch_ok      = false;
static bool     touch_pressed = false;
static uint16_t touch_x       = 0;
static uint16_t touch_y       = 0;

void touch_hal_init(void) {
    uint8_t gt_addr = GT911_ADDR_ALT;
    Wire.beginTransmission(GT911_ADDR_PRIMARY);
    if (Wire.endTransmission() == 0) gt_addr = GT911_ADDR_PRIMARY;

    touch.setPins(-1, -1);  // no RST, no INT on this board
    // No pins: board_init() already started Wire, and SensorLib would
    // call Wire.setPins() on the running bus (an [E] log line).
    if (!touch.begin(Wire, gt_addr)) {
        Serial.printf("Touch GT911 init failed (tried 0x%02X and 0x%02X)\n",
                      GT911_ADDR_PRIMARY, GT911_ADDR_ALT);
        return;
    }

    touch.setMaxCoordinates(LCD_WIDTH, LCD_HEIGHT);
    touch.setSwapXY(false);
    touch.setMirrorXY(false, false);

    touch_ok = true;
    Serial.printf("Touch GT911 init OK (addr 0x%02X)\n", gt_addr);
}

void touch_hal_read(uint16_t* x, uint16_t* y, bool* pressed) {
    if (touch_ok) {
        const TouchPoints &pts = touch.getTouchPoints();
        if (pts.hasPoints()) {
            touch_pressed = true;
            touch_x = pts.getPoint(0).x;
            touch_y = pts.getPoint(0).y;
        } else {
            touch_pressed = false;
        }
    }
    *x = touch_x;
    *y = touch_y;
    *pressed = touch_pressed;
}
