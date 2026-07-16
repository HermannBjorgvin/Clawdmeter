#pragma once

#include <stdint.h>

// This panel needs MY for portrait scan direction and BGR color order.
// Setting MX as the generic Adafruit rotation-0 path does mirror the image
// horizontally on the ESP32-2432S024C.
constexpr uint8_t st7789_portrait_bgr_madctl(void) {
    return 0x80 | 0x08;
}
