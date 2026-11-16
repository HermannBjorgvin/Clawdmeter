#include "../../hal/touch_hal.h"
#include "board.h"
#include <Arduino.h>
#include <Wire.h>

// Built for the 2.1 (CST820) or, with BOARD_LCD28C, the 2.8C (GT911).

#ifdef BOARD_LCD28C

#include <TouchDrvGoodix.hpp>

// GT911 via SensorLib, the same driver the LCD-4 and 4848S040 ports use.
// TP_RST is expander EXIO2, so SensorLib gets no pins: board_init() already
// pulsed the reset with INT held low (address 0x5D). Without pins,
// SensorLib's begin() probes 0x5D then 0x14 itself whatever address it is
// given, so the log reports the address it settled on. The controller is
// polled. The Waveshare demo uses no swap or mirror.

static TouchDrvGT911 touch;

static bool     touch_ok      = false;
static bool     touch_pressed = false;
static uint16_t touch_x       = 0;
static uint16_t touch_y       = 0;

void touch_hal_init(void) {
    touch.setPins(-1, -1);
    // No pins passed to begin(): board_init() already started Wire, and
    // SensorLib would call Wire.setPins() on the running bus (an [E] log).
    if (!touch.begin(Wire, GT911_ADDR_PRIMARY)) {
        Serial.printf("Touch GT911 init failed (SensorLib probed 0x%02X and 0x%02X)\n",
                      GT911_ADDR_PRIMARY, GT911_ADDR_ALT);
        return;
    }

    touch.setMaxCoordinates(LCD_WIDTH, LCD_HEIGHT);
    touch.setSwapXY(false);
    touch.setMirrorXY(false, false);

    touch_ok = true;
    Serial.printf("Touch GT911 init OK (addr 0x%02X)\n", touch.getAddress());
}

void touch_hal_read(uint16_t* x, uint16_t* y, bool* pressed) {
    if (touch_ok) {
        const TouchPoints &pts = touch.getTouchPoints();
        if (pts.hasPoints()) {
            touch_pressed = true;
            uint16_t px = pts.getPoint(0).x;
            uint16_t py = pts.getPoint(0).y;
            touch_x = px >= LCD_WIDTH  ? LCD_WIDTH - 1  : px;
            touch_y = py >= LCD_HEIGHT ? LCD_HEIGHT - 1 : py;
        } else {
            touch_pressed = false;
        }
    }
    *x = touch_x;
    *y = touch_y;
    *pressed = touch_pressed;
}

#else  // LCD-2.1

// Minimal CST820 reader, same register layout as the AMOLED-1.32 port:
//   reg 0x02: low nibble = finger count; 0x03/0x04 = X hi/lo; 0x05/0x06 = Y hi/lo
// Read only when TP_INT has fired. TP_RST is expander EXIO2, pulsed in
// board_init(). The Waveshare demo uses no swap or mirror.

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
    touch_x = px >= LCD_WIDTH  ? LCD_WIDTH - 1  : px;
    touch_y = py >= LCD_HEIGHT ? LCD_HEIGHT - 1 : py;
    touch_pressed = true;
}

void touch_hal_init(void) {
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

#endif  // BOARD_LCD28C
