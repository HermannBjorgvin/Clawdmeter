#include "../../hal/touch_hal.h"
#include "board.h"
#include "../../hal/imu_hal.h"
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

// display.cpp rotates LVGL's strips in software (rotate_strip), but the
// CST9220 keeps reporting in the panel's unrotated frame. Map the point back
// through the inverse of rotate_strip so hit-tests line up in every quadrant.
static void to_display_frame(uint16_t* x, uint16_t* y) {
    const uint16_t N = LCD_WIDTH - 1;   // square panel
    const uint16_t tx = *x > N ? N : *x, ty = *y > N ? N : *y;
    switch (imu_hal_rotation_quadrant()) {
    case 1: *x = ty;     *y = N - tx; break;   // drawn (x,y) -> (N-y, x)
    case 2: *x = N - tx; *y = N - ty; break;   // drawn (x,y) -> (N-x, N-y)
    case 3: *x = N - ty; *y = tx;     break;   // drawn (x,y) -> (y, N-x)
    default: break;
    }
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
    *x = touch_x;
    *y = touch_y;
    to_display_frame(x, y);
    *pressed = touch_pressed;
}
