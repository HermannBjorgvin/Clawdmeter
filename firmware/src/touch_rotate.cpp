#include "touch_rotate.h"
#include "hal/board_caps.h"

// Fixed touch-controller -> physical-panel alignment (quadrant-independent).
// The CST9220 is configured with swap+mirror (touch.cpp), which lands its output
// a quarter turn off the panel; this single transform undoes that so the value we
// hand LVGL is in the panel's own frame. LVGL's lv_display_rotate_point() then
// applies the per-orientation rotation. (First-pass value; tune here if a specific
// orientation is uniformly off — it's ONE transform for all four quadrants now.)
void touch_to_panel(uint16_t raw_x, uint16_t raw_y, uint16_t* out_x, uint16_t* out_y) {
    const int S = board_caps().width;   // square panel
    *out_x = raw_y;
    *out_y = S - 1 - raw_x;
}

