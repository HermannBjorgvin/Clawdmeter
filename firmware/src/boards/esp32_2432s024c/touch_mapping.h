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
