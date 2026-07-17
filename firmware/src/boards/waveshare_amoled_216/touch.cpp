#include "../../hal/touch_hal.h"
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
            // The setSwapXY/setMirrorXY pair above leaves the CST9220 reporting
            // 90° CW from the panel's own frame. Measured, not guessed: tapping
            // the four screen corners reported (v, W-1-u) against what was
            // displayed — a quarter turn. It went unnoticed because the only
            // touch handler is a full-screen tap, so no hit-test ever depended
            // on the coordinate being right.
            // Undo it here so touch_hal_read returns true panel coordinates —
            // the frame the HAL contract implies and the rotation math assumes.
            const uint16_t ox = (uint16_t)tx[0], oy = (uint16_t)ty[0];
            touch_x = oy;
            touch_y = (uint16_t)(LCD_WIDTH - 1) - ox;
        } else {
            touch_pressed = false;
        }
    }
    *x = touch_x;
    *y = touch_y;
    *pressed = touch_pressed;
}
