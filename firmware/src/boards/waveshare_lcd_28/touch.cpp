#include "../../hal/touch_hal.h"
#include "board.h"
#include <Arduino.h>
#include <Wire.h>
#include <TouchDrvGoodix.hpp>

// GT911 via SensorLib, the same driver the LCD-4 and 4848S040 ports use.
// TP_RST is expander EXIO2, so SensorLib gets no pins: board_init() already
// pulsed the reset with INT held low (address 0x5D). 0x14 is the fallback in
// case the latch didn't take. The controller is polled. The Waveshare demo
// uses no swap or mirror.

static TouchDrvGT911 touch;

static bool     touch_ok      = false;
static bool     touch_pressed = false;
static uint16_t touch_x       = 0;
static uint16_t touch_y       = 0;

void touch_hal_init(void) {
    uint8_t gt_addr = GT911_ADDR_ALT;
    Wire.beginTransmission(GT911_ADDR_PRIMARY);
    if (Wire.endTransmission() == 0) gt_addr = GT911_ADDR_PRIMARY;

    touch.setPins(-1, -1);
    // No pins passed to begin(): board_init() already started Wire, and
    // SensorLib would call Wire.setPins() on the running bus (an [E] log).
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
            uint16_t px = pts.getPoint(0).x;
            uint16_t py = pts.getPoint(0).y;
            touch_x = px >= LCD_WIDTH  ? LCD_WIDTH - 1  : px;
            touch_y = py >= LCD_HEIGHT ? LCD_HEIGHT - 1 : py;
        } else {
            touch_pressed = false;
        }
    }
    *x = touch_x;
    *y = touch_y;
    *pressed = touch_pressed;
}
