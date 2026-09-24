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

// Re-bind to the art set for the current theme mode. Call after
// theme_set_mode(): the two art sets have different animations, different
// stage sizes and different rate groups, so the cached group lists and the
// current animation index are stale the moment the mode changes.
void splash_reload_art(void);

// Which pet the Codex side shows. Persisted, so the choice survives a reboot
// the way the provider mode does. Cycling rebinds the art set and restarts
// whatever was playing.
void splash_pet_next(void);
const char* splash_pet_name(void);

// Freeze every mascot: the splash animation, the corner mascot's acts and its
// trips all hold their current frame. Persisted, since the point of quieting
// a desk display is that it stays quiet.
void splash_set_paused(bool paused);
bool splash_paused(void);

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

// Corner mascot (usage screen, PSRAM boards): the still Clawd idles in the
// logo slot, does occasional acts, and takes walk-off/lurk/walk-back trips.
// feet_y = px of the art's ground line; cell = px per art cell in the corner.
// Corner mascot. `slot_px` is the height available and `max_cell` the largest
// px-per-art-cell worth using; the mascot fits itself to the slot and refits
// whenever the art set changes. The two sets are drawn at very different
// heights -- Clawd's still pose is ~16 cells, Codey's ~30 -- so a cell size
// chosen once, for whichever set happened to be active at boot, overflows the
// slot with the other.
lv_obj_t* splash_mascot_create(lv_obj_t *parent, int slot_x, int feet_y,
                               int slot_px, int max_cell);
void splash_mascot_tick(void);
void splash_mascot_set_visible(bool v);
