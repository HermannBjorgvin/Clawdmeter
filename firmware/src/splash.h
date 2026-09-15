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

// Override the mood group from the Claude-activity signal (0..3), or -1 to
// fall back to the usage-rate group. Re-picks immediately if the splash is up.
void splash_set_activity(int group);

// True when splash is currently rendering (used to gate re-picks).
bool splash_is_active(void);

// Root container (so ui.cpp can attach a click event).
lv_obj_t* splash_get_root(void);

// Copilot screen pixel-art animation (60x60, 3x scale). Shares a single
// buffer with the splash and Aurora canvases (see splash.cpp) since only
// one of the three is ever visible at a time.
// splash_copilot_init() creates the canvas widget inside `parent`.
// splash_copilot_show() re-renders the first frame fresh — call every time
// the Copilot screen becomes visible, since the shared buffer may hold
// stale content from whichever of the three was shown last.
// splash_copilot_tick() advances the frame if the hold time has elapsed.
void splash_copilot_init(lv_obj_t* parent);
void splash_copilot_show(void);
void splash_copilot_tick(void);

// Aurora screen pixel-art animation ("aurora shimmer") — same shared-buffer
// pattern as Copilot above.
void splash_aurora_init(lv_obj_t* parent);
void splash_aurora_show(void);
void splash_aurora_tick(void);
