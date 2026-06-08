#pragma once
#include <stdint.h>
#include <lvgl.h>

// Initialize splash module. Creates the canvas widget inside `parent` and
// allocates the 480x480 pixel buffer (PSRAM).
void splash_init(lv_obj_t *parent);

// Advance animation frame if hold time elapsed. Call from main loop.
void splash_tick(void);

// Cycle to the next animation in the catalog.
void splash_next(void);

// Show/hide the splash container.
void splash_show(void);
void splash_hide(void);

// Pick the next animation matching the current usage-rate group.
// Called automatically by splash_show(); also exposed so other modules can
// trigger a re-pick when the rate group changes mid-display.
void splash_pick_for_current_rate(void);

// True when splash is currently rendering (used to gate re-picks).
bool splash_is_active(void);

// Root container (so ui.cpp can attach a click event).
lv_obj_t* splash_get_root(void);

// ---- Mini-animation rendering (reused by the Enterprise screen) ----
// These let other screens draw the 20x20 pixel-art animations at a small size
// without duplicating the (large, static) animation tables into another module.

// Number of animations available in the catalog.
int splash_anim_count(void);

// Render animation `idx`, frame `frame` (wrapped to the animation's frame_count)
// into `dst` — an RGB565 buffer sized (20*cell) x (20*cell). Returns that
// animation's frame_count (0 if idx is out of range or dst is null).
int splash_render_into(int idx, int frame, uint16_t* dst, int cell);

// Hold time in ms for animation `idx`, frame `frame` (wrapped). 0 if invalid.
uint16_t splash_frame_hold(int idx, int frame);

// Mini animated creature for embedding elsewhere (e.g. the idle screen).
// Renders the named claudepix animation (e.g. "expression sleep") at ~px×px
// inside `parent`; returns the canvas object (position it with lv_obj_align) or
// NULL if the animation isn't found / allocation fails. Drive it with
// splash_mini_tick(). One mini creature at a time.
lv_obj_t* splash_mini_create(lv_obj_t *parent, const char *anim_name, int px);
void splash_mini_tick(void);
