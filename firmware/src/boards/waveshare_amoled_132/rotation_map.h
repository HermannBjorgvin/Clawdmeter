#pragma once
#include <stdint.h>

// Point mappings for the button-driven UI rotation on a square S×S panel.
// rot_forward matches display.cpp's rotate_strip(); rot_inverse is what
// touch.cpp applies to panel coordinates. Pure so it can be host-tested.

static inline void rot_forward(uint8_t q, uint16_t S, uint16_t x, uint16_t y,
                               uint16_t* px, uint16_t* py) {
    switch (q & 3) {
    case 1:  *px = S - 1 - y; *py = x;         break;   // 90° CW
    case 2:  *px = S - 1 - x; *py = S - 1 - y; break;   // 180°
    case 3:  *px = y;         *py = S - 1 - x; break;   // 270° CW
    default: *px = x;         *py = y;         break;
    }
}

static inline void rot_inverse(uint8_t q, uint16_t S, uint16_t px, uint16_t py,
                               uint16_t* x, uint16_t* y) {
    switch (q & 3) {
    case 1:  *x = py;         *y = S - 1 - px; break;
    case 2:  *x = S - 1 - px; *y = S - 1 - py; break;
    case 3:  *x = S - 1 - py; *y = px;         break;
    default: *x = px;         *y = py;         break;
    }
}
