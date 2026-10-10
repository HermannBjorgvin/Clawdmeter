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

// Mini animated creature for embedding elsewhere (e.g. the idle screen).
// Renders the named official animation (e.g. "cloud") at ~px×px
// inside `parent`; returns the canvas object (position it with lv_obj_align) or
// NULL if the animation isn't found / allocation fails. Drive it with
// splash_mini_tick(). One mini creature at a time.
lv_obj_t* splash_mini_create(lv_obj_t *parent, const char *anim_name, int px);
void splash_mini_tick(void);

// Corner mascot (usage screen): the still Clawd idles in the logo slot. On
// PSRAM boards he also does occasional acts and takes walk-off/lurk/walk-back
// trips on his own; on all boards a tap on him plays the next act.
// feet_y = px of the art's ground line; cell = px per art cell in the corner.
// Returns NULL if the buffer can't be allocated (caller falls back to a still).
lv_obj_t* splash_mascot_create(lv_obj_t *parent, int slot_x, int feet_y, int cell);
void splash_mascot_tick(void);
void splash_mascot_set_visible(bool v);
// Attention: the corner Clawd walks off, a full-size one walks in and waves
// and points until attention(false), then they swap back. No-op without the
// PSRAM mascot.
void splash_mascot_attention(bool on);

// Screen-px tap. If it lands inside the mascot's bounding box, starts the next
// tap act (dance → scuttle off-left and back → wave) when he's idle, and
// returns true so the caller swallows the tap. Taps on him mid-act are
// swallowed too. Returns false for taps anywhere else.
bool splash_mascot_tap(int x, int y);
