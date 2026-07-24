#pragma once
#include <stdint.h>

// Orientation-aware touch, LVGL-owned rotation model.
//
// The panel auto-rotates the DISPLAY by quadrant (display.cpp rotate_strip), and
// we mirror that into LVGL via lv_display_set_rotation(). LVGL then rotates touch
// input for hit-testing on its own (lv_indev -> lv_display_rotate_point), so there
// is NO per-quadrant touch table — just one fixed alignment:
//
//   touch_to_panel() - a single, quadrant-independent alignment from the touch
//                      controller's output to the physical panel frame. We hand
//                      this to LVGL, which applies the per-orientation rotation.
//
// Swipes are recognised by a background gesture handler in ui.cpp using LVGL's
// already-rotated point, so nothing here deals with gestures. Boards that don't
// auto-rotate report quadrant 0, so this is identity there.

void touch_to_panel(uint16_t raw_x, uint16_t raw_y, uint16_t* out_x, uint16_t* out_y);
