#pragma once
#include <stdint.h>
#include "data.h"
#include "signals.h"

// The director: one Clawd, on the meters, driven by the situation.
//
//   meters  home and default; every path ends here. The corner Clawd sits in
//           his seat and does small acts (mascot.h). He also performs here,
//           full size in front of them (performer.h): a treat after a busy
//           stretch, or a calm dip during the screensaver.
//   face    the #198 face resting on the glass (face.h): the screensaver,
//           and the peek when you tap the meters.
//
// Rules of continuity: only ever one Clawd (corner sprite, full-size sprite,
// performer or face); he enters and leaves through an edge; the screen never
// hard-cuts. A tap during a performance or on the face means "show me the
// meters"; on the meters themselves it's tapping the aquarium: he reacts.
//
// Beats (design system §09): notice, show, tap → meters, screensaver + wake,
// attention, fresh-window joy, long-turn wave, and the tap-on-the-meters
// peek. The lurk trip and idle acts are the corner's own secondary action.
// Minimal mode collapses all of it to a still Clawd. See
// docs/clawd-director.md.

enum DirectorRoom { ROOM_METERS, ROOM_FACE };

void director_init(void);          // after ui_init()
void director_tick(void);

// Per parsed payload, after signals_ingest().
void director_on_payload(const UsageData* d, const SignalsUpdate* sig);
void director_alert(const char* message);   // attention hook fired
void director_alert_clear(void);            // the session moved on
bool director_take_dismissed(void);         // an alert ended on the device since the last call

// Any touch press at screen x (wired internally to LVGL). On the meters it's
// tapping the aquarium: the face comes up, curious, for a while (a tap on it
// sends it back); anywhere else it means "show me the meters".
void director_tap(int x);
// While on, the panel is the pairing control (the touch-hold re-pair on
// boards without a PWR button): a tap on the meters still counts as you
// being here and dismisses an alert, but Clawd doesn't peek, so the hint
// stays in view for the hold.
void director_set_pairing_touch(bool on);
// A physical button press: you're here. `main_button` is BOOT, or the PWR
// key on a board whose only button it is. On a board without a touchscreen
// that press first dismisses an active alert and returns true (the caller
// then skips the button's normal action, push to talk or brightness);
// otherwise false.
bool director_button(bool main_button);
// Apply a liveliness level (clawd_policy.h) now: the tuning changes, and
// what the new level rules out (a show, the screensaver, idle acts, a
// pending reaction) stops.
void director_set_liveliness(int level);

// How long Clawd's attention animation runs, in seconds (clawd_policy.h;
// 0 = off). The alert itself is unchanged.
void director_set_attn_anim(int seconds);

// Print every beat step with a timestamp (the sim turns it on with
// SIM_TRACE=1 for walkthrough timings). Beat changes always print.
void director_set_trace(bool on);

DirectorRoom director_room(void);
const char*  director_beat_name(void);

// QA hook for the `clawd <beat>` serial command: start a beat now so it can
// be captured with `screenshot` on hardware. beat = show | screensaver |
// attention | joy | lurk | peek [x] | home. Returns false for an unknown
// name, or for show / screensaver / lurk / peek while another beat is still
// running (lurk also needs the full-size sprite, i.e. PSRAM). `peek` is a tap
// on the meters at x (default: three quarters across).
bool director_debug(const char* beat);
