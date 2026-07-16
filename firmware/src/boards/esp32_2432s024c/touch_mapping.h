#pragma once

#include <stdint.h>

struct TouchPoint {
    uint16_t x;
    uint16_t y;
};

inline TouchPoint map_touch_to_portrait(uint16_t raw_x, uint16_t raw_y) {
    return {
        static_cast<uint16_t>(raw_x > 239 ? 239 : raw_x),
        static_cast<uint16_t>(raw_y > 319 ? 319 : raw_y),
    };
}

inline TouchPoint map_touch_to_landscape(uint16_t raw_x, uint16_t raw_y) {
    const uint16_t x = raw_x > 239 ? 239 : raw_x;
    const uint16_t y = raw_y > 319 ? 319 : raw_y;
    return {
        static_cast<uint16_t>(319 - y),
        x,
    };
}
