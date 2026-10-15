#include "../../hal/touch_hal.h"
#include "board.h"
#include "io_expander.h"
#include <Arduino.h>
#include <Wire.h>

// Minimal FT6x36 I2C reader (FocalTech standard register layout):
//   reg 0x02: low nibble = active finger count
//   reg 0x03 / 0x04: X1 high (low nibble) + X1 low
//   reg 0x05 / 0x06: Y1 high (low nibble) + Y1 low
// Panel mounted 180° — both axes flipped in touch_hal_read().

static bool     touch_init_ok = false;
static bool     touch_pressed = false;
static uint16_t touch_x       = 0;
static uint16_t touch_y       = 0;
static uint32_t last_read_ms  = 0;

static void ft6x36_read(void) {
    Wire.beginTransmission(SENSECAP_TOUCH_ADDR);
    Wire.write(0x02);
    if (Wire.endTransmission(false) != 0) { touch_pressed = false; return; }
    if (Wire.requestFrom(SENSECAP_TOUCH_ADDR, (uint8_t)5) != 5) { touch_pressed = false; return; }

    uint8_t fingers = Wire.read() & 0x0F;
    uint8_t xH = Wire.read();
    uint8_t xL = Wire.read();
    uint8_t yH = Wire.read();
    uint8_t yL = Wire.read();

    if (fingers == 0 || fingers > 5) { touch_pressed = false; return; }

    uint16_t raw_x = ((uint16_t)(xH & 0x0F) << 8) | xL;
    uint16_t raw_y = ((uint16_t)(yH & 0x0F) << 8) | yL;
    // Flip both axes: panel mounted 180° relative to logical (0,0) top-left.
    touch_x = (LCD_WIDTH  - 1) - raw_x;
    touch_y = (LCD_HEIGHT - 1) - raw_y;
    touch_pressed = true;
}

void touch_hal_init(void) {
    // RST pulse: pull LOW for 50 ms, release, wait 300 ms for controller to boot.
    io_expander_set(SENSECAP_TP_RST_PIN, false);
    delay(50);
    io_expander_set(SENSECAP_TP_RST_PIN, true);
    delay(300);

    // Set active scanning mode (register 0xA5 = 0x00).
    Wire.beginTransmission(SENSECAP_TOUCH_ADDR);
    Wire.write(0xA5);
    Wire.write(0x00);
    if (Wire.endTransmission() == 0) {
        touch_init_ok = true;
        Serial.println("FT6x36 touch init OK");
    } else {
        Serial.printf("FT6x36 not found at 0x%02X — touch disabled\n",
                      SENSECAP_TOUCH_ADDR);
    }
}

void touch_hal_read(uint16_t* x, uint16_t* y, bool* pressed) {
    if (!touch_init_ok) { *x = 0; *y = 0; *pressed = false; return; }

    uint32_t now = millis();
    if (now - last_read_ms >= 20) {   // 50 Hz poll — stays well under 5 ms budget
        last_read_ms = now;
        ft6x36_read();
    }
    *x = touch_x;
    *y = touch_y;
    *pressed = touch_pressed;
}
