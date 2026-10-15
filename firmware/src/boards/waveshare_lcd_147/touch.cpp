#include "../../hal/touch_hal.h"
#include "board.h"
#include <Arduino.h>
#include <Wire.h>

// Minimal AXS5106L reader, vendored (like the FocalTech/CST readers on the
// other ports) to keep the dependency tree copyleft-free and avoid pulling in
// Waveshare's esp_lcd_touch_axs5106l shim.
//
// Protocol (from Waveshare's driver for this exact kit):
//   reg 0x01: 14-byte burst — data[1] = finger count, then 6 bytes per finger:
//             data[2] X high nibble, data[3] X low, data[4] Y high nibble,
//             data[5] Y low, (+2 bytes we ignore)
//   reg 0x08: chip id (3 bytes), used only as a presence probe
// The controller raises INT (falling) once per report while a finger is down.
//
// Coordinates come out in the panel's native 172x320 portrait frame. The UI
// runs landscape (LCD_ROTATION 1), so the axes are swapped with no mirroring
// — the mapping Waveshare's own driver applies for rotation 1.

#define AXS5106L_ID_REG         0x08
#define AXS5106L_TOUCH_REG      0x01

static volatile bool     touch_data_ready = false;
static volatile bool     touch_pressed = false;
static volatile uint16_t touch_x = 0;
static volatile uint16_t touch_y = 0;

static void IRAM_ATTR touch_isr(void) {
    touch_data_ready = true;
}

static void touch_read_into_shared_state(void) {
    Wire.beginTransmission(AXS5106L_ADDR);
    Wire.write(AXS5106L_TOUCH_REG);
    if (Wire.endTransmission() != 0) { touch_pressed = false; return; }

    uint8_t data[14] = {0};
    if (Wire.requestFrom((uint8_t)AXS5106L_ADDR, (uint8_t)sizeof(data)) != sizeof(data)) {
        touch_pressed = false;
        return;
    }
    Wire.readBytes(data, sizeof(data));

    uint8_t fingers = data[1];
    if (fingers == 0 || fingers > 5) {
        touch_pressed = false;
        return;
    }

    uint16_t raw_x = ((uint16_t)(data[2] & 0x0F) << 8) | data[3];   // 0..171
    uint16_t raw_y = ((uint16_t)(data[4] & 0x0F) << 8) | data[5];   // 0..319

#if LCD_ROTATION == 1
    // 90° — swap axes, no mirror.
    touch_x = raw_y;
    touch_y = raw_x;
#elif LCD_ROTATION == 3
    // 270° — swap axes and mirror both.
    touch_x = (LCD_PANEL_H - 1) - raw_y;
    touch_y = (LCD_PANEL_W - 1) - raw_x;
#else
    // 0° portrait — mirror X only (the controller's native reading is flipped
    // against the panel on this kit).
    touch_x = (LCD_PANEL_W - 1) - raw_x;
    touch_y = raw_y;
#endif
    touch_pressed = true;
}

void touch_hal_init(void) {
    // Hardware reset. The AXS5106L needs a long, clean pulse before it
    // answers on I2C — Waveshare's driver uses 200 ms low / 300 ms settle
    // and shorter pulses were not reliable.
    pinMode(TP_RST, OUTPUT);
    digitalWrite(TP_RST, LOW);
    delay(200);
    digitalWrite(TP_RST, HIGH);
    delay(300);

    uint8_t id[3] = {0};
    Wire.beginTransmission(AXS5106L_ADDR);
    Wire.write(AXS5106L_ID_REG);
    if (Wire.endTransmission() == 0 &&
        Wire.requestFrom((uint8_t)AXS5106L_ADDR, (uint8_t)3) == 3) {
        Wire.readBytes(id, 3);
        Serial.printf("Touch AXS5106L ID=%02X %02X %02X (addr 0x%02X)\n",
                      id[0], id[1], id[2], AXS5106L_ADDR);
    } else {
        Serial.printf("Touch ID read failed (addr 0x%02X)\n", AXS5106L_ADDR);
    }

    pinMode(TP_INT, INPUT_PULLUP);
    attachInterrupt(TP_INT, touch_isr, FALLING);
    Serial.println("Touch attached on INT pin");
}

void touch_hal_read(uint16_t* x, uint16_t* y, bool* pressed) {
    if (touch_data_ready) {
        touch_data_ready = false;
        touch_read_into_shared_state();
    } else if (touch_pressed) {
        // The finger-up report can land between polls; re-read while we still
        // believe a finger is down so a stuck "pressed" state clears.
        touch_read_into_shared_state();
    }
    *x = touch_x;
    *y = touch_y;
    *pressed = touch_pressed;
}
