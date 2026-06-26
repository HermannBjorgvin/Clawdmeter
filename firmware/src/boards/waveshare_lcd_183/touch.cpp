#include "../../hal/touch_hal.h"
#include "board.h"
#include <Arduino.h>
#include <Wire.h>

// CST816 capacitive touch. Same FocalTech-style data layout as the AMOLED-1.8
// reader (only this board has a single fixed controller, so no board_rev):
//   reg 0x02:        low nibble = active finger count
//   reg 0x03 / 0x04: X1 high (low nibble) + X1 low
//   reg 0x05 / 0x06: Y1 high (low nibble) + Y1 low

static volatile bool     touch_data_ready = false;
static volatile bool     touch_pressed = false;
static volatile uint16_t touch_x = 0;
static volatile uint16_t touch_y = 0;

static void IRAM_ATTR touch_isr(void) {
    touch_data_ready = true;
}

static void touch_read_into_shared_state(void) {
    Wire.beginTransmission(CST816_ADDR);
    Wire.write(0x02);
    if (Wire.endTransmission(false) != 0) { touch_pressed = false; return; }
    if (Wire.requestFrom((uint8_t)CST816_ADDR, (uint8_t)5) != 5) { touch_pressed = false; return; }
    uint8_t fingers = Wire.read() & 0x0F;
    uint8_t xH = Wire.read();
    uint8_t xL = Wire.read();
    uint8_t yH = Wire.read();
    uint8_t yL = Wire.read();
    if (fingers == 0 || fingers > 5) {
        touch_pressed = false;
        return;
    }
    // ponytail: direct mapping. If taps land mirrored/swapped on the panel,
    // flip here — e.g. touch_x = LCD_WIDTH-1-x, or swap x/y. Calibrate on first boot.
    touch_x = ((uint16_t)(xH & 0x0F) << 8) | xL;
    touch_y = ((uint16_t)(yH & 0x0F) << 8) | yL;
    touch_pressed = true;
}

void touch_hal_init(void) {
    // Verify the controller answers (CST816 chip-id is reg 0xA7).
    Wire.beginTransmission(CST816_ADDR);
    Wire.write(0xA7);
    if (Wire.endTransmission(false) == 0 && Wire.requestFrom((uint8_t)CST816_ADDR, (uint8_t)1) == 1) {
        Serial.printf("Touch CST816 ID=0x%02X (addr 0x%02X)\n", Wire.read(), CST816_ADDR);
    } else {
        Serial.printf("Touch ID read failed (addr 0x%02X)\n", CST816_ADDR);
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
