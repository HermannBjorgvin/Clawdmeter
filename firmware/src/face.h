#pragma once
#include <lvgl.h>

// Clawd face screen: the whole panel in Clawd orange with two black eyes that
// blink at random into a "> <" squint. Meant for boards mounted in an orange
// Clawd-shaped shell. LVGL objects only — no pixel buffers, so it's cheap on
// PSRAM-less boards.

// Build the face (hidden) as a full-screen child of `parent`. Returns its root
// so the caller can attach input handlers.
lv_obj_t* face_init(lv_obj_t *parent);

// Show with eyes open and start the random blink timer / hide and stop it.
void face_show(void);
void face_hide(void);

lv_obj_t* face_get_root(void);
