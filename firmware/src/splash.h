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

// Cycle to the previous animation in the catalog.
void splash_prev(void);

// Pause/resume rendering (e.g. when a different screen is active).
void splash_set_active(bool active);

// Show/hide the splash container.
void splash_show(void);
void splash_hide(void);

// Root container (so ui.cpp can attach a click event).
lv_obj_t* splash_get_root(void);

// Current animation name (for debug/UI).
const char *splash_current_name(void);

uint16_t splash_count(void);
