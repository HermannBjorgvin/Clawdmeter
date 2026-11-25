#pragma once
#include <stdint.h>

// What a BoardCaps.mono_panel board prints. Its driver thresholds every RGB565
// pixel LVGL renders on BT.601 luminance (Q8 coefficients 76/150/30, 0-255):
// MONO_INK_LUMA or more prints as ink, anything darker as paper. The dark
// default palette therefore comes out black on white: the background (0),
// cards (26) and bar track (40) are paper; red (94), green (127), amber and
// accent (141), secondary text (171) and text (247) are ink. 64 sits between
// the brightest background and the darkest foreground. Pure, so the native
// tests and the simulator's SIM_MONO=1 preview use the same rule.
#define MONO_INK_LUMA 64

static inline uint8_t mono_luma565(uint16_t p) {
    const uint32_t r = (p >> 11) & 0x1f, g = (p >> 5) & 0x3f, b = p & 0x1f;
    return (uint8_t)(((r << 3) * 76 + (g << 2) * 150 + (b << 3) * 30) >> 8);
}

static inline bool mono_ink565(uint16_t p) { return mono_luma565(p) >= MONO_INK_LUMA; }
