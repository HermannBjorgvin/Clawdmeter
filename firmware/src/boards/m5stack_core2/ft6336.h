#pragma once
#include <stdint.h>

// The FT6336U driver. Only touch.cpp (touch_hal_read, called every LVGL
// refresh, asleep or not) runs ft6336_poll(), so the controller is read in one
// place (firmware/CLAUDE.md gotcha 5); input.cpp (A, C) and power.cpp (B)
// read the latched zone. A touch that starts in the strip below the panel is
// a button press for as long as the finger stays down; one that starts on the
// panel is a screen touch, so dragging across the edge never turns into a button.

enum TouchZone { TZ_NONE = 0, TZ_SCREEN, TZ_BTN_A, TZ_BTN_B, TZ_BTN_C };

bool ft6336_init(void);
// Re-reads the controller at most every 15 ms (and re-probes it once a second
// until it answers). Called only from touch_hal_read().
void ft6336_poll(void);
TouchZone ft6336_zone(void);
uint16_t ft6336_x(void);
uint16_t ft6336_y(void);
