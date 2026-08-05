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

// Mini animated creature for embedding elsewhere (the idle "Zzz" panel, the
// corner badge on the usage screen). Each instance owns its canvas, buffer and
// frame clock, so several can run at once.
//
// `anim_name` picks a claudepix animation by name (e.g. "expression sleep");
// pass NULL to follow the live usage-rate group instead, re-picking on the same
// cadence as the full-screen splash. Renders at ~px×px inside `parent`.
// Returns NULL if the animation isn't found or allocation fails.
//
// Position the instance via splash_mini_canvas(); drive it with
// splash_mini_tick(). Both are NULL-safe.
typedef struct splash_mini splash_mini_t;

splash_mini_t* splash_mini_create(lv_obj_t *parent, const char *anim_name, int px);
lv_obj_t*      splash_mini_canvas(splash_mini_t *m);
void           splash_mini_tick(splash_mini_t *m);
