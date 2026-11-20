#pragma once
#include <stdint.h>

// One FT6336U poller shared by touch.cpp (the screen), input.cpp (A, C) and
// power.cpp (B). A touch that starts in the strip below the panel is a button
// press for as long as the finger stays down; one that starts on the panel is
// a screen touch, so dragging across the edge never turns into a button.

enum TouchZone { TZ_NONE = 0, TZ_SCREEN, TZ_BTN_A, TZ_BTN_B, TZ_BTN_C };

bool ft6336_init(void);
// Re-reads the controller at most every 15 ms; cheap to call from every HAL.
void ft6336_poll(void);
TouchZone ft6336_zone(void);
uint16_t ft6336_x(void);
uint16_t ft6336_y(void);
