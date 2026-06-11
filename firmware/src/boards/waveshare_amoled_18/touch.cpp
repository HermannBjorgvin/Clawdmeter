#include "../../hal/touch_hal.h"
#include "board.h"
#include "board_rev.h"
#include <Arduino.h>
#include <Wire.h>

// Minimal capacitive-touch reader. Both shipping panel revisions expose the
// FocalTech-style touch-data layout, so one read path serves both — only the
// I2C address differs (FT3168 @ 0x38 vs CST816 @ 0x15), chosen from board_rev().
//   reg 0x02:        low nibble = active finger count
//   reg 0x03 / 0x04: X1 high (low nibble) + X1 low
//   reg 0x05 / 0x06: Y1 high (low nibble) + Y1 low

static volatile bool     touch_data_ready = false;
static volatile bool     touch_pressed = false;
static volatile uint16_t touch_x = 0;
static volatile uint16_t touch_y = 0;
static uint8_t           touch_addr = FT3168_ADDR;

static void IRAM_ATTR touch_isr(void) {
    touch_data_ready = true;
}

static void touch_read_into_shared_state(void) {
    Wire.beginTransmission(touch_addr);
    Wire.write(0x02);
    if (Wire.endTransmission(false) != 0) { touch_pressed = false; return; }
    if (Wire.requestFrom(touch_addr, (uint8_t)5) != 5) { touch_pressed = false; return; }
    uint8_t fingers = Wire.read() & 0x0F;
    uint8_t xH = Wire.read();
    uint8_t xL = Wire.read();
    uint8_t yH = Wire.read();
    uint8_t yL = Wire.read();
    if (fingers == 0 || fingers > 5) {
        touch_pressed = false;
        return;
    }
    touch_x = ((uint16_t)(xH & 0x0F) << 8) | xL;
    touch_y = ((uint16_t)(yH & 0x0F) << 8) | yL;
    touch_pressed = true;
}

void touch_hal_init(void) {
    bool is_cst816 = (board_rev() == REV_CO5300_CST816);
    touch_addr = is_cst816 ? CST816_ADDR : FT3168_ADDR;

    if (!is_cst816) {
        // FT3168 power-mode register 0xA5 = 0x00: active scanning.
        // (CST816 reports by default; no equivalent setup needed.)
        Wire.beginTransmission(touch_addr);
        Wire.write(0xA5);
        Wire.write(0x00);
        Wire.endTransmission();
    }

    // Verify the controller answers. FT3168 chip-id is reg 0xA0; CST816 reg 0xA7.
    uint8_t id_reg = is_cst816 ? 0xA7 : 0xA0;
    Wire.beginTransmission(touch_addr);
    Wire.write(id_reg);
    if (Wire.endTransmission(false) == 0 && Wire.requestFrom(touch_addr, (uint8_t)1) == 1) {
        Serial.printf("Touch %s ID=0x%02X (addr 0x%02X)\n",
                      is_cst816 ? "CST816" : "FT3168", Wire.read(), touch_addr);
    } else {
        Serial.printf("Touch ID read failed (addr 0x%02X)\n", touch_addr);
    }

    pinMode(TP_INT, INPUT_PULLUP);
    attachInterrupt(TP_INT, touch_isr, FALLING);
    Serial.println("Touch attached on INT pin");
}

void touch_hal_read(uint16_t* x, uint16_t* y, bool* pressed) {
    if (touch_data_ready) {
        touch_data_ready = false;
        touch_read_into_shared_state();
    }
    *x = touch_x;
    *y = touch_y;
    *pressed = touch_pressed;
}
