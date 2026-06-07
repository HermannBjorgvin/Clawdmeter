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

// Working-mode: while Claude Code is actively running the splash "homes" to the
// "work coding" animation. The PWR button can still cycle animations, but the
// splash snaps back to "work coding" a few seconds after the last manual cycle
// (while still working). Driven by the daemon's "working" flag.
void splash_set_working(bool working);
void splash_note_manual(void);   // call when the user manually cycles (PWR button)
bool splash_get_working(void);

// Mini animated creature for embedding elsewhere (e.g. the idle screen, or a
// small "working" badge on the usage meter). Each instance is caller-owned via
// a splash_mini_t, so several can run at once. Renders the named claudepix
// animation at ~px×px inside `parent`; returns the canvas object (position it
// with lv_obj_set_pos/lv_obj_align) or NULL if the animation isn't found /
// allocation fails. Drive it with splash_mini_tick(&handle).
typedef struct {
    int       anim_idx;   // index into the animation catalog; -1 = none
    uint16_t* buf;
    lv_obj_t* canvas;
    uint16_t  frame;
    uint32_t  started;
    int       cell;
    int       w;
} splash_mini_t;

lv_obj_t* splash_mini_create(splash_mini_t* m, lv_obj_t* parent, const char* anim_name, int px);
void splash_mini_tick(splash_mini_t* m);
