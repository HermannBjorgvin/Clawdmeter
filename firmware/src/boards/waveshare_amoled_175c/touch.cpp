#include "../../hal/touch_hal.h"
#include "board.h"
#include <Arduino.h>
#include <Wire.h>
#include <TouchDrvCST.hpp>

static TouchDrvCST92xx touch;
static volatile bool touch_data_ready = false;
static bool touch_pressed = false;
static uint16_t touch_x = 0;
static uint16_t touch_y = 0;

static void IRAM_ATTR touch_isr(void) {
    touch_data_ready = true;
}

void touch_hal_init(void) {
    touch.setPins(TP_RST, TP_INT);
    if (!touch.begin(Wire, CST9220_ADDR)) {
        Serial.println("Touch init failed");
        return;
    }
    touch.setMaxCoordinates(LCD_WIDTH, LCD_HEIGHT);
    touch.setMirrorXY(true, true);
    pinMode(TP_INT, INPUT_PULLUP);
    attachInterrupt(TP_INT, touch_isr, FALLING);
    Serial.println("Touch init OK");
}

void touch_hal_read(uint16_t* x, uint16_t* y, bool* pressed) {
    if (touch_data_ready) {
        touch_data_ready = false;
        const TouchPoints& pts = touch.getTouchPoints();
        touch_pressed = pts.hasPoints();
        if (touch_pressed) {
            touch_x = pts.getPoint(0).x;
            touch_y = pts.getPoint(0).y;
        }
    }
    *x = touch_x;
    *y = touch_y;
    *pressed = touch_pressed;
}
