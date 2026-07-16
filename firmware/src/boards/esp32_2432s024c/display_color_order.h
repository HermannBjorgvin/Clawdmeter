#pragma once

#include <stdint.h>

// Rotation 0 uses MX|MY. This panel's ST7789 is wired for BGR color order,
// so MADCTL bit 3 must also be set or red and blue are exchanged.
constexpr uint8_t st7789_portrait_bgr_madctl(void) {
    return 0x80 | 0x40 | 0x08;
}
