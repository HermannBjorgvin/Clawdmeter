#include "../../hal/touch_hal.h"
#include "board.h"
#include <Arduino.h>
#include <Wire.h>
#include <TouchDrvGT911.hpp>

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
        int16_t tx[5], ty[5];
        uint8_t n = touch.getPoint(tx, ty, touch.getSupportTouchPoint());
        if (n > 0) {
            touch_pressed = true;
            // Panel → logical: inverse of display.cpp's LCD_ROTATION remap.
            const int16_t px = tx[0], py = ty[0];
            switch (LCD_ROTATION) {
            case 1:  touch_x = py;                  touch_y = LCD_WIDTH - 1 - px;  break;
            case 2:  touch_x = LCD_WIDTH - 1 - px;  touch_y = LCD_HEIGHT - 1 - py; break;
            case 3:  touch_x = LCD_HEIGHT - 1 - py; touch_y = px;                  break;
            default: touch_x = px;                  touch_y = py;                  break;
            }
        } else {
            touch_pressed = false;
        }
    }
    *x = touch_x;
    *y = touch_y;
    *pressed = touch_pressed;
}
