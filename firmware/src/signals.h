#pragma once
#include <stdint.h>
#include "data.h"

// Signals: the one place a parsed payload turns into "what just happened" for
// Clawd. Owns the fast clock (hook events, activity, active sessions) and feeds
// the slow clock (usage_rate). The director (rooms, face, shows) reads these
// instead of looking at UsageData fields directly.
//
// Call order per payload: payload_parse() → signals_ingest() → act on the
// returned SignalsUpdate. Pure core (the *_at functions) is unit-tested in
// test/test_signals; the plain wrappers read millis().
//
// Backward compatibility: with an old daemon (no a/e/es/n/rp) nothing here
// fires except session_reset and group changes, signals_hooks_seen() stays
// false, and every getter returns its "unknown" value.

struct SignalsUpdate {
    bool     session_reset;   // a real sample dropped > 5 points (chime trigger)
    bool     replay;          // payload was a heartbeat replay (rp=1)
    bool     event_fired;     // a hook event the device hasn't reacted to yet
    char     event;           // 'p' prompt, 's' stop, 'b' back after a gap,
                              // or a future char; valid when event_fired
    uint32_t event_seq;       // the `es` that fired
    bool     sessions_changed;// `n` changed (including first sight)
    int      group_before;    // usage_rate group before this payload
    int      group_after;     // ... and after (activity lift included)
};

// Event dedupe rule: the device reacts once per distinct `es`. The first `es`
// seen after boot only sets the baseline (it describes something that
// happened before we were listening), and replays never fire. Any change in
// `es` fires, not just an increase, so a daemon restart that resets its
// counter is still noticed.

void          signals_clear(void);   // boot state; for tests
SignalsUpdate signals_ingest_at(const UsageData* d, uint32_t now_ms);

// True once any fast-clock field (a, e/es, n) has arrived since boot, i.e.
// the daemon is hook-aware and the hooks are installed. Shows / screensaver /
// "you're back" should only run when this is true; without it Clawd runs on
// the slow clock alone (design system §08 "Hooks are optional").
bool     signals_hooks_seen(void);

int      signals_activity_at(uint32_t now_ms);    // 0-3, -1 unknown/expired
int      signals_sessions(void);                  // -1 unknown
char     signals_last_event(void);                // '\0' none since boot

// ---- millis() wrappers (usage_rate_clock.cpp) ----
SignalsUpdate signals_ingest(const UsageData* d);
int      signals_activity(void);
