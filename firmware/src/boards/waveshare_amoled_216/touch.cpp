#include "../../hal/touch_hal.h"
#include "../../hal/imu_hal.h"
#include "board.h"
#include <Arduino.h>
#include <Wire.h>
#include <TouchDrvCSTXXX.hpp>

static TouchDrvCST92xx touch;

static volatile bool     touch_data_ready = false;
static volatile bool     touch_pressed = false;
static volatile uint16_t touch_x = 0;
static volatile uint16_t touch_y = 0;

static void IRAM_ATTR touch_isr(void) {
    touch_data_ready = true;
}

void touch_hal_init(void) {
    touch.setPins(TP_RST, TP_INT);
    if (!touch.begin(Wire, CST9220_ADDR, IIC_SDA, IIC_SCL)) {
        Serial.println("Touch init failed");
        return;
    }
    touch.setMaxCoordinates(LCD_WIDTH, LCD_HEIGHT);
    touch.setSwapXY(true);
    touch.setMirrorXY(true, false);
    pinMode(TP_INT, INPUT_PULLUP);
    attachInterrupt(TP_INT, touch_isr, FALLING);
    Serial.println("Touch init OK");
}

void touch_hal_read(uint16_t* x, uint16_t* y, bool* pressed) {
    if (touch_data_ready) {
        touch_data_ready = false;
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

    // The panel is rotated in software (see display.cpp), so raw touches need
    // the matching transform or taps miss their target when rotated. It isn't
    // the clean inverse of the display rotation (controller swap/mirror
    // calibration) — measured on-device as transform = 3 - quadrant.
    const int S = LCD_WIDTH;  // square panel: width == height
    uint16_t px = touch_x, py = touch_y;
    switch (3 - (imu_hal_rotation_quadrant() & 3)) {
    case 1: *x = S - 1 - py; *y = px;          break;
    case 2: *x = S - 1 - px; *y = S - 1 - py;  break;
    case 3: *x = py;         *y = S - 1 - px;  break;
    default: *x = px;        *y = py;          break;
    }
    *pressed = touch_pressed;
}
