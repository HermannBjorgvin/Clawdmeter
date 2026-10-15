#include "../../hal/touch_hal.h"
#include "board.h"
#include "board_rev.h"
#include <Arduino.h>
#include <Wire.h>

// Minimal capacitive-touch reader. Both shipping revisions expose the
// FocalTech-style data layout, so one read path serves both — only the I2C
// address differs (FT3168 @ 0x38 vs CST820 @ 0x15). CST820 is Hynitron CST8xx,
// register-compatible with the CST816 the 1.8 port reads.
//   reg 0x02:        low nibble = active finger count
//   reg 0x03 / 0x04: X1 high (low nibble) + X1 low
//   reg 0x05 / 0x06: Y1 high (low nibble) + Y1 low
//
// No INT pin is wired in this port, so we poll on every read. A 5-byte burst
// at 400 kHz is ~150 us, comfortably inside LVGL's per-refresh touch budget.

static uint8_t  touch_addr = CST816_ADDR;

static bool     last_pressed = false;
static uint16_t last_x = 0;
static uint16_t last_y = 0;

static void touch_poll(void) {
    Wire.beginTransmission(touch_addr);
    Wire.write(0x02);
    if (Wire.endTransmission(false) != 0) { last_pressed = false; return; }
    if (Wire.requestFrom(touch_addr, (uint8_t)5) != 5) { last_pressed = false; return; }
    uint8_t fingers = Wire.read() & 0x0F;
    uint8_t xH = Wire.read();
    uint8_t xL = Wire.read();
    uint8_t yH = Wire.read();
    uint8_t yL = Wire.read();
    if (fingers == 0 || fingers > 5) { last_pressed = false; return; }
    // Raw controller coordinates. The display is left un-rotated (panel mounts
    // 180°; the device is physically rotated in use — see display.cpp), so touch
    // is left raw to match. If you ever software-rotate the display 180°, mirror
    // both axes here too: last_x = LCD_WIDTH-1-x; last_y = LCD_HEIGHT-1-y.
    last_x = ((uint16_t)(xH & 0x0F) << 8) | xL;
    last_y = ((uint16_t)(yH & 0x0F) << 8) | yL;
    last_pressed = true;
}

void touch_hal_init(void) {
    bool is_cst = (board_rev() == REV_CO5300_CST816);
    touch_addr = is_cst ? CST816_ADDR : FT3168_ADDR;

    if (!is_cst) {
        // FT3168 power-mode register 0xA5 = 0x00: active scanning.
        // CST820 reports by default; no equivalent setup needed.
        Wire.beginTransmission(touch_addr);
        Wire.write(0xA5);
        Wire.write(0x00);
        Wire.endTransmission();
    }

    uint8_t id_reg = is_cst ? 0xA7 : 0xA0;
    Wire.beginTransmission(touch_addr);
    Wire.write(id_reg);
    if (Wire.endTransmission(false) == 0 && Wire.requestFrom(touch_addr, (uint8_t)1) == 1) {
        Serial.printf("Touch %s ID=0x%02X (addr 0x%02X)\n",
                      is_cst ? "CST820" : "FT3168", Wire.read(), touch_addr);
    } else {
        Serial.printf("Touch ID read failed (addr 0x%02X)\n", touch_addr);
    }
    Serial.println("Touch: polled (no INT pin)");
}

void touch_hal_read(uint16_t* x, uint16_t* y, bool* pressed) {
    touch_poll();
    *x = last_x;
    *y = last_y;
    *pressed = last_pressed;
}

// --- Optional INT-driven path -------------------------------------------------
// If you wire the touch INT line to a GPIO, define TP_INT in board.h and use
// this instead of polling: attach a FALLING ISR that sets a flag, and only call
// touch_poll() from touch_hal_read() when the flag is set. Mirrors the 1.8 port.
