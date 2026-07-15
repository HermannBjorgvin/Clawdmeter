#include "../../hal/touch_hal.h"
#include "board.h"
#include <Arduino.h>
#include <Wire.h>

// CST816 capacitive touch over I2C. The CST816 exposes the FocalTech-style
// data layout, so this is the same minimal reader used by the AMOLED-1.8 port
// (vendored to avoid the GPLv3 Arduino_DriveBus library):
//   reg 0x02:        low nibble = active finger count
//   reg 0x03 / 0x04: X1 high (low nibble) + X1 low
//   reg 0x05 / 0x06: Y1 high (low nibble) + Y1 low
//
// If X/Y come out swapped or mirrored on hardware, fix it HERE (swap touch_x/
// touch_y, or mirror against LCD_WIDTH/LCD_HEIGHT) so LVGL sees panel-space
// coordinates — see docs/porting/hal-contract.md.

static volatile bool     touch_data_ready = false;
static volatile bool     touch_pressed = false;
static volatile uint16_t touch_x = 0;
static volatile uint16_t touch_y = 0;

static void IRAM_ATTR touch_isr(void) {
    touch_data_ready = true;
}

static void touch_read_into_shared_state(void) {
    Wire.beginTransmission(TP_ADDR);
    Wire.write(0x02);
    if (Wire.endTransmission(false) != 0) { touch_pressed = false; return; }
    if (Wire.requestFrom((uint8_t)TP_ADDR, (uint8_t)5) != 5) { touch_pressed = false; return; }
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
#if TP_RST >= 0
    pinMode(TP_RST, OUTPUT);
    digitalWrite(TP_RST, LOW);
    delay(10);
    digitalWrite(TP_RST, HIGH);
    delay(50);   // CST816 needs ~50ms after reset before it ACKs
#endif

    // CST816 chip-id lives at reg 0xA7 — probe it so boot logs confirm the bus.
    Wire.beginTransmission(TP_ADDR);
    Wire.write(0xA7);
    if (Wire.endTransmission(false) == 0 && Wire.requestFrom((uint8_t)TP_ADDR, (uint8_t)1) == 1) {
        Serial.printf("Touch CST816 ID=0x%02X (addr 0x%02X)\n", Wire.read(), TP_ADDR);
    } else {
        Serial.printf("Touch ID read failed (addr 0x%02X)\n", TP_ADDR);
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
