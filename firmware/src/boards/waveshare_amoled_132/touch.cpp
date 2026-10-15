#include "../../hal/touch_hal.h"
#include "../../hal/imu_hal.h"
#include "board.h"
#include "rotation_map.h"
#include <Arduino.h>
#include <Wire.h>

// Minimal CST820 reader (same FocalTech-style layout as the 1.8 port's CST816):
//   reg 0x02: low nibble = finger count; 0x03/0x04 = X hi/lo; 0x05/0x06 = Y hi/lo
// Read only when TP_INT has fired, then map panel coords back through the
// current UI rotation so LVGL sees logical coordinates.

static volatile bool touch_data_ready = false;
static bool     touch_pressed = false;
static uint16_t touch_x = 0;
static uint16_t touch_y = 0;

static void IRAM_ATTR touch_isr(void) { touch_data_ready = true; }

static void touch_poll(void) {
    Wire.beginTransmission(CST820_ADDR);
    Wire.write(0x02);
    if (Wire.endTransmission(false) != 0 ||
        Wire.requestFrom((uint8_t)CST820_ADDR, (uint8_t)5) != 5) {
        touch_pressed = false;
        return;
    }
    uint8_t fingers = Wire.read() & 0x0F;
    uint8_t xH = Wire.read(), xL = Wire.read();
    uint8_t yH = Wire.read(), yL = Wire.read();
    if (fingers == 0 || fingers > 5) { touch_pressed = false; return; }

    uint16_t px = ((uint16_t)(xH & 0x0F) << 8) | xL;
    uint16_t py = ((uint16_t)(yH & 0x0F) << 8) | yL;
    if (px >= LCD_WIDTH)  px = LCD_WIDTH - 1;
    if (py >= LCD_HEIGHT) py = LCD_HEIGHT - 1;
    rot_inverse(imu_hal_rotation_quadrant(), LCD_WIDTH, px, py, &touch_x, &touch_y);
    touch_pressed = true;
}

void touch_hal_init(void) {
    pinMode(TP_RST, OUTPUT);
    digitalWrite(TP_RST, LOW);
    delay(20);
    digitalWrite(TP_RST, HIGH);
    delay(100);

    Wire.beginTransmission(CST820_ADDR);
    Wire.write(0xA7);   // chip ID
    if (Wire.endTransmission(false) == 0 &&
        Wire.requestFrom((uint8_t)CST820_ADDR, (uint8_t)1) == 1) {
        Serial.printf("Touch CST820 ID=0x%02X\n", Wire.read());
    } else {
        Serial.println("Touch init failed (no ACK @ 0x15)");
    }

    pinMode(TP_INT, INPUT_PULLUP);
    attachInterrupt(TP_INT, touch_isr, FALLING);
}

void touch_hal_read(uint16_t* x, uint16_t* y, bool* pressed) {
    // CST8xx pulses INT while a finger is down and stops on lift, so poll once
    // more after the last pulse to observe the release.
    if (touch_data_ready || touch_pressed) {
        touch_data_ready = false;
        touch_poll();
    }
    *x = touch_x;
    *y = touch_y;
    *pressed = touch_pressed;
}
