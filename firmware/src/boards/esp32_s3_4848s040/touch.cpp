#include "../../hal/touch_hal.h"
#include "board.h"
#include <Arduino.h>
#include <Wire.h>
#include <TouchDrvGT911.hpp>

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
    if (!touch.begin(Wire, gt_addr, IIC_SDA, IIC_SCL)) {
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
        int16_t tx[5], ty[5];
        uint8_t n = touch.getPoint(tx, ty, touch.getSupportTouchPoint());
        if (n > 0) {
            touch_pressed = true;
            touch_x = (uint16_t)tx[0];
            touch_y = (uint16_t)ty[0];
        } else {
            touch_pressed = false;
        }
    }
    *x = touch_x;
    *y = touch_y;
    *pressed = touch_pressed;
}
