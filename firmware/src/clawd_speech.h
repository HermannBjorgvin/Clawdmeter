#pragma once
#include <stdint.h>
#include <stddef.h>
#include <lvgl.h>

// ─────────────────────────────────────────────────────────────────────────────
// Clawd's speech bubble — the mascot's voice.
//
// Two roles:
//   1. A general FEEDBACK VECTOR: call clawd_speak()/clawd_speak_fmt() from
//      anywhere to have the mascot surface a short message (e.g. "Session
//      refilled!"). The bubble pops beside Clawd and auto-fades.
//   2. A tap easter egg: clawd_quip() composes a SITUATIONALLY-AWARE line from
//      the live stats you pass in a ClawdContext, weighted so he usually comments
//      on what's actually happening, with generic flavor as a rare treat.
//
// The module is theme- and app-agnostic: styling is passed at init, and the live
// data arrives via ClawdContext, so it never reaches into ui.cpp internals.
//
// To ADD OR EDIT what Clawd says, see clawd_collect_quips() in clawd_speech.cpp —
// it's one well-marked function; add a line with add(weight, "text", args...).
// ─────────────────────────────────────────────────────────────────────────────

// Live situation the quips react to. Fill what you have; use the sentinels for
// anything unknown. Add a field here + read it in clawd_collect_quips() to give
// Clawd more to notice.
typedef struct {
    float       session_pct;        // 0..100
    float       weekly_pct;         // 0..100
    float       burn_rate;          // session %/hr — valid only if burn_known
    bool        burn_known;         // false when there isn't enough history yet
    int         session_reset_min;  // minutes to the 5h reset, or -1 if unknown
    const char* costume;            // current costume name, or NULL if bare
    int         hour;               // local hour 0..23, or -1 if no wall-clock
    bool        connected;          // daemon/BLE link up
} ClawdContext;

// Visual config, passed once so the module stays theme-agnostic.
typedef struct {
    const lv_font_t* font;
    lv_color_t       bubble_bg;     // bubble fill
    lv_color_t       text_color;    // text on the bubble
    int32_t          max_width;     // wrap width for long lines (px)
} ClawdSpeechStyle;

// Create the bubble (hidden), anchored just below `mascot`. Call once at UI init.
void clawd_speech_init(lv_obj_t* parent, lv_obj_t* mascot, const ClawdSpeechStyle* style);

// Say arbitrary text — the general feedback vector. Auto-fades (dwell scales with
// length). No-op while the mascot itself is hidden (e.g. on the splash).
void clawd_speak(const char* text);
void clawd_speak_fmt(const char* fmt, ...);

// Compose + say a context-aware quip for the given situation (mascot tap).
void clawd_quip(const ClawdContext* ctx);

// How often Clawd speaks up on his own (unprompted). QUIET = never; BALANCED and
// CHATTY auto-speak on a jittered timer. Tapping him always works regardless.
typedef enum { CLAWD_QUIET = 0, CLAWD_BALANCED = 1, CLAWD_CHATTY = 2 } ClawdChattiness;
void            clawd_speech_set_chattiness(ClawdChattiness mode);
ClawdChattiness clawd_speech_get_chattiness(void);

// Register a callback that fills a ClawdContext with the current situation. Used
// for autonomous speech (the module has no other way to know the live stats).
void clawd_speech_set_context_provider(void (*provider)(ClawdContext* out));

void clawd_speech_tick(void);   // auto-fade + autonomous chatter (call each loop)
void clawd_speech_hide(void);   // hide immediately (e.g. on a page change)
