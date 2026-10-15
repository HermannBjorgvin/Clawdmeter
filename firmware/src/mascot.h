#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <lvgl.h>

// Clawd on the meters: the corner Clawd in his seat (3 px cells on a 480
// panel) plus the full-size sprite he becomes for the lurk trip (8 px) and
// the attention walk-on (12 px). Same character, one at a time: the big one
// only appears after the small one has walked off an edge.
//
// The director (clawd_director.cpp) commands him; on his own he only does the
// in-room secondary action while seated and `autonomy` is on: jittered idle
// gaps, mood-picked acts (never the same twice running) and the lurk trip.
//
// The corner sprite works on every board (its ~23 KB buffer falls back to
// internal RAM); the full-size sprite needs PSRAM (~250 KB). Without it the
// lurk trip is skipped and attention is a wave and point from his seat.

lv_obj_t* mascot_create(lv_obj_t *parent, int slot_x, int feet_y, int cell);
void mascot_tick(void);

bool mascot_seated(void);            // in his seat, between acts
bool mascot_away(void);              // fully off screen
bool mascot_busy(void);              // walking, acting, lurking or fetching you
bool mascot_acting(void);            // playing an act in his seat
bool mascot_free(void);              // seated, or in an idle act of his own (a new act may cut in)
bool mascot_out(void);               // out of his seat on his own: lurk trip, walk, away

void mascot_autonomy(bool on);       // idle acts + lurk trips while seated
// Play `anim` in the seat `times` times (noticing, a wave, jumping happy).
// Interrupts an idle act; refused (false) while he's walking or away. An act
// too tall for the headroom above the seat plays a shorter official act.
// snappy: skip the idle lead-in frame (every act opens on the idle pose, held
// ~330 ms) so a reaction to a tap shows on the next frame. speed_pct scales
// the authored timing (a startle plays a little quick).
bool mascot_act(const char* anim, uint8_t times, bool snappy = false, uint16_t speed_pct = 100);
void mascot_leave(uint16_t speed_pct);                 // walk off the nearest edge (a lurk finishes, no slower, and he stays out)
void mascot_return(int from_side, uint16_t speed_pct); // -1 left / +1 right edge → seat
// Something to react to while he's out of his seat: cut a lurk trip short
// (the rest of the peek plays fast), turn around if he's heading out, or
// speed up a walk home. True when he's home or on his way; false while the
// attention Clawd owns him.
bool mascot_hurry_home(void);
bool mascot_lurk(void);              // QA: start a lurk trip now (seated, PSRAM only)
void mascot_place_away(void);        // off screen now (a curtain is covering)
void mascot_reset(void);             // seated now, nothing pending

// Attention: corner walks off left at 2x, the big Clawd walks on at 2x and
// alternates waving and pointing until attention(false), then walks off and
// the small one walks home. Without the full-size sprite he waves and points
// in his seat instead (walking back to it first if he's out). Minimal mode:
// a held pointing frame in the seat.
// to_seat=false (only meaningful with on=false): the small one stays off
// screen after the big one leaves, for a screensaver that resumes.
void mascot_attention(bool on, bool to_seat = true);
bool mascot_in_attention(void);      // anything of the attention beat still on screen

void mascot_set_minimal(bool on);    // still frame only, no autonomy
