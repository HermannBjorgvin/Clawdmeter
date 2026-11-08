#include "../../hal/touch_hal.h"
#include "board.h"
#include <Arduino.h>
#include <Wire.h>
#include <TouchDrvCST.hpp>

// CST816D via SensorLib's CST816 driver. It reports coordinates already
// aligned with the ST7789 at rotation 0, so no axis swap or mirror is applied.

static TouchDrvCST816 touch;

static volatile bool     touch_data_ready = false;
static volatile bool     touch_pressed = false;
static volatile uint16_t touch_x = 0;
static volatile uint16_t touch_y = 0;

static void IRAM_ATTR touch_isr(void) {
    touch_data_ready = true;
}

static void touch_poll(void) {
    const TouchPoints &pts = touch.getTouchPoints();
    if (pts.hasPoints()) {
        touch_pressed = true;
        touch_x = pts.getPoint(0).x;
        touch_y = pts.getPoint(0).y;
    } else {
        touch_pressed = false;
    }
}

void touch_hal_init(void) {
    touch.setPins(TP_RST, TP_INT);
    // No pins: board_init() already started Wire, and SensorLib would
    // call Wire.setPins() on the running bus (an [E] log line).
    if (!touch.begin(Wire, TOUCH_ADDR)) {
        Serial.println("Touch init failed");
        return;
    }
    touch.setMaxCoordinates(LCD_WIDTH, LCD_HEIGHT);
    pinMode(TP_INT, INPUT_PULLUP);
    attachInterrupt(TP_INT, touch_isr, FALLING);
    Serial.println("Touch init OK");
}

void touch_hal_read(uint16_t* x, uint16_t* y, bool* pressed) {
    if (touch_data_ready) {
        touch_data_ready = false;
        touch_poll();
    } else if (touch_pressed) {
        // INT fires per report while a finger is down, but the finger-up
        // report can land between polls: re-read while we think we are
        // pressed so a stuck "pressed" state clears (the 5 s pairing hold
        // and long presses depend on a clean release).
        touch_poll();
    }
    *x = touch_x;
    *y = touch_y;
    *pressed = touch_pressed;
}
