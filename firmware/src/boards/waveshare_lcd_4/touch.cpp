#include "../../hal/touch_hal.h"
#include "board.h"
#include <Arduino.h>
#include <Wire.h>
#include <TouchDrvGoodix.hpp>

// GT911 via SensorLib. Address is latched from INT at reset — probe 0x5D
// then fall back to 0x14. Polled (no IRQ): Waveshare's INT pin is GPIO 16,
// left unused because polling is reliable.

static TouchDrvGT911 touch;

static bool     touch_ok      = false;
static bool     touch_pressed = false;
static uint16_t touch_x       = 0;
static uint16_t touch_y       = 0;

void touch_hal_init(void) {
    uint8_t gt_addr = 0x14;
    Wire.beginTransmission(0x5D);
    if (Wire.endTransmission() == 0) gt_addr = 0x5D;

    touch.setPins(-1, -1);
    if (!touch.begin(Wire, gt_addr, IIC_SDA, IIC_SCL)) {
        Serial.printf("Touch GT911 init failed (tried 0x%02X)\n", gt_addr);
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
