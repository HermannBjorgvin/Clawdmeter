#include "../../hal/touch_hal.h"
#include "board.h"

#include <Arduino.h>
#include <Wire.h>

static bool read_touch_sample(uint8_t* data, size_t size) {
    Wire.beginTransmission(TP_ADDR);
    Wire.write(0x01);
    if (Wire.endTransmission(false) != 0) {
        return false;
    }

    if (Wire.requestFrom(TP_ADDR, static_cast<uint8_t>(size)) != size) {
        return false;
    }

    for (size_t index = 0; index < size; ++index) {
        data[index] = Wire.read();
    }
    return true;
}

void touch_hal_init(void) {
    Wire.beginTransmission(TP_ADDR);
    Wire.write(0xA7);
    if (Wire.endTransmission(false) == 0 &&
        Wire.requestFrom(TP_ADDR, static_cast<uint8_t>(1)) == 1) {
        Serial.printf("Touch CST816/CST820 ID=0x%02X (addr 0x%02X)\n",
                      Wire.read(), TP_ADDR);
        return;
    }

    Serial.printf("Touch probe failed (addr 0x%02X)\n", TP_ADDR);
}

void touch_hal_read(uint16_t* x, uint16_t* y, bool* pressed) {
    uint8_t data[6] = {};
    if (!read_touch_sample(data, sizeof(data))) {
        *pressed = false;
        return;
    }

    const uint8_t fingers = data[1] & 0x0F;
    if (fingers == 0 || fingers > 5) {
        *pressed = false;
        return;
    }

    const uint16_t raw_x =
        (static_cast<uint16_t>(data[2] & 0x0F) << 8) | data[3];
    const uint16_t raw_y =
        (static_cast<uint16_t>(data[4] & 0x0F) << 8) | data[5];
#ifdef BOARD_LANDSCAPE
    *x = 319 - min(raw_y, static_cast<uint16_t>(319));
    *y = 239 - min(raw_x, static_cast<uint16_t>(239));
#else
    *x = min(raw_x, static_cast<uint16_t>(239));
    *y = min(raw_y, static_cast<uint16_t>(319));
#endif
    *pressed = true;
}
