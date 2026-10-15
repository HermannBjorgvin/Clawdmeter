#pragma once
#include <stdbool.h>
#include <stdint.h>

// The #198 face: Clawd up close, resting on the glass. Clawd-orange full
// field, two tall dark eyes; a blink is the solid-cell > < squint; on a glance
// the far eye travels 1.5x. A field on lv_layer_top() (field.h) with the
// eyes as small LVGL rectangles on it — no pixel buffer — so it runs on every
// board, PSRAM or not. Rising and dropping redraw only the rows the field
// gains or loses plus the eyes, not the whole face, every frame.
//
// It's Clawd come up to the glass: the screensaver, and the peek when you
// tap the meters. It rises from behind the bottom bezel and drops back below it,
// straight up and down with a flat top edge, arms on the rise, squash and
// stretch and follow-through (face_motion.h). Owned and sequenced by the
// director.

void face_init(void);
void face_tick(void);

void face_rise(void);          // up from the bottom bezel, overshoot, settle
void face_rise_toward(int x);  // the same, curious about screen x (a peek): resting glances go back there
void face_drop(void);          // anticipation hop, then down behind the bezel; hidden when gone
void face_hide_now(void);      // no animation (mode switch / reset only)

bool face_visible(void);       // any part of it on screen
bool face_moving(void);        // rising (until settled) or dropping
bool face_busy(void);          // moving, blinking or glancing

void face_blink(bool twice);   // the noticing beat
void face_set_resting(bool on);// irregular blinks + glances while up
// Look toward screen x (a tap): the eyes glance proportionally (up to 1.5x
// the resting glance, far eye 1.5x on top), hold for hold_ms, then come back
// to you. face_busy() until they're back.
void face_look_at_x(int x, uint32_t hold_ms);
