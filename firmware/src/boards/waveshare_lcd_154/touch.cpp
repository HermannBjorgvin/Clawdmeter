#include "../../hal/touch_hal.h"
#include "board.h"
#include <Arduino.h>
#include <Wire.h>

// Minimal CST816T reader — same FocalTech-style register layout as the
// AMOLED-1.8 port's inline reader (regs 0x02..0x06), vendored to keep the
// dependency tree copyleft-free:
//   reg 0x02:        low nibble = active finger count
//   reg 0x03 / 0x04: X high (low nibble) + X low
//   reg 0x05 / 0x06: Y high (low nibble) + Y low
// The CST816T reports in panel-native orientation on this kit — no axis swap
// or mirror at rotation 0 (matches BambuHelper's hardware-tested config for
// the same board, which applies no swap flags).

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
    touch_x = ((uint16_t)(xH & 0x0F) << 8) | xL;
    touch_y = ((uint16_t)(yH & 0x0F) << 8) | yL;
    touch_pressed = true;
}

void touch_hal_init(void) {
    // Hardware reset — the CST816T needs a clean reset pulse before it
    // responds on I2C (it powers up in a low-power state otherwise).
    pinMode(TP_RST, OUTPUT);
    digitalWrite(TP_RST, LOW);
    delay(10);
    digitalWrite(TP_RST, HIGH);
    delay(100);

    // Verify the controller answers. CST816 chip-id lives at reg 0xA7.
    Wire.beginTransmission(CST816_ADDR);
    Wire.write(0xA7);
    if (Wire.endTransmission(false) == 0 &&
        Wire.requestFrom((uint8_t)CST816_ADDR, (uint8_t)1) == 1) {
        Serial.printf("Touch CST816 ID=0x%02X (addr 0x%02X)\n", Wire.read(), CST816_ADDR);
    } else {
        Serial.printf("Touch ID read failed (addr 0x%02X)\n", CST816_ADDR);
    }

    // Disable the controller's auto-sleep — asleep it stops raising INT and
    // the first tap after idle would be swallowed. Reg 0xFE nonzero = stay on.
    Wire.beginTransmission(CST816_ADDR);
    Wire.write(0xFE);
    Wire.write(0x01);
    Wire.endTransmission();

    pinMode(TP_INT, INPUT_PULLUP);
    attachInterrupt(TP_INT, touch_isr, FALLING);
    Serial.println("Touch attached on INT pin");
}

void touch_hal_read(uint16_t* x, uint16_t* y, bool* pressed) {
    if (touch_data_ready) {
        touch_data_ready = false;
        touch_read_into_shared_state();
    } else if (touch_pressed) {
        // CST816 raises INT per report while a finger is down, but the release
        // (finger-up) report can be missed if it lands between polls — re-read
        // while we think we're pressed so a stuck "pressed" state clears.
        touch_read_into_shared_state();
    }
    *x = touch_x;
    *y = touch_y;
    *pressed = touch_pressed;
}
