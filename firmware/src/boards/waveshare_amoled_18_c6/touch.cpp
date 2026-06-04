#include "../../hal/touch_hal.h"
#include "board.h"
#include <Arduino.h>
#include <Wire.h>

// Minimal capacitive-touch reader for the FT3168 (some units ship an FT6146;
// both are FocalTech parts at 0x38 with the same data layout, so one path
// serves both). Mirrors the S3 1.8 inline reader — touch reset is handled by
// the TCA9554 touch-power line, so there's no reset pin to drive here.
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
    Wire.beginTransmission(FT3168_ADDR);
    Wire.write(0x02);
    if (Wire.endTransmission(false) != 0) { touch_pressed = false; return; }
    if (Wire.requestFrom(FT3168_ADDR, (uint8_t)5) != 5) { touch_pressed = false; return; }
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
    // FT3168 power-mode register 0xA5 = 0x00: active scanning.
    Wire.beginTransmission(FT3168_ADDR);
    Wire.write(0xA5);
    Wire.write(0x00);
    Wire.endTransmission();

    // Verify the controller answers (FT3168 chip-id is reg 0xA0).
    Wire.beginTransmission(FT3168_ADDR);
    Wire.write(0xA0);
    if (Wire.endTransmission(false) == 0 && Wire.requestFrom(FT3168_ADDR, (uint8_t)1) == 1) {
        Serial.printf("Touch FT3168 ID=0x%02X (addr 0x%02X)\n", Wire.read(), FT3168_ADDR);
    } else {
        Serial.printf("Touch ID read failed (addr 0x%02X)\n", FT3168_ADDR);
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
