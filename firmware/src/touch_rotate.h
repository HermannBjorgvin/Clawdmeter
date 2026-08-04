#pragma once
#include <stdint.h>

// Orientation-aware touch, LVGL-owned rotation model.
//
// The AMOLED-2.16 auto-rotates the DISPLAY by IMU quadrant (the CO5300 can't
// rotate in hardware, so display.cpp CPU-rotates the flush via rotate_strip), but
// the touch controller keeps reporting in its fixed native frame. Left alone, taps
// land in the wrong place whenever the device is turned.
//
// The fix is to mirror the display rotation into LVGL via lv_display_set_rotation()
// (see main.cpp) and let LVGL rotate the touch point for hit-testing on its own
// (lv_indev -> lv_display_rotate_point). Then all that's left here is one fixed,
// quadrant-independent alignment from the touch controller's output to the panel
// frame — LVGL applies the per-orientation rotation on top. Boards that don't
// auto-rotate report quadrant 0, so this stays an identity there.

void touch_to_panel(uint16_t raw_x, uint16_t raw_y, uint16_t* out_x, uint16_t* out_y);
