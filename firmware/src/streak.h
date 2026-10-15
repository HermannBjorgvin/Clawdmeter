#pragma once
#include <stdbool.h>
#include <lvgl.h>

// The orange streak: a 150 px (on a 480 panel) Clawd-orange band that sweeps
// right to left (~380 ms, ease in/out) over the stage and reveals the meters
// behind it. A field on lv_layer_top() (field.h: plain LVGL drawing, no
// pixel buffer, so it works without PSRAM; each frame redraws only the
// columns the band gained and lost). While it runs, the stage room is clipped to the streak's left side
// (`cover` is narrowed), so the meters show to its right.

void streak_init(void);
void streak_run(lv_obj_t* cover);   // cover = the stage room's root object
bool streak_running(void);
void streak_cancel(void);           // hide now and restore the cover
