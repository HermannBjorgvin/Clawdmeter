#pragma once
#include <stdint.h>

// Plain data, no Arduino types, so payload parsing and the signal logic can be
// unit-tested on the host (test/test_signals).

struct UsageData {
    float session_pct;       // utilization 0-100 (5h window Pro/Max; spending % Enterprise)
    int session_reset_mins;  // minutes until reset
    float weekly_pct;        // 7-day utilization (Pro/Max only; 0 for Enterprise)
    int weekly_reset_mins;   // minutes until weekly reset (Pro/Max only)
    char status[16];         // "allowed", "limited", etc.
    bool chime;              // play the session-reset chime; false unless the app opts in
    bool beep;               // Claude needs attention: play the chime once for this packet
    char alert_msg[128];     // attention message for the status line; empty = no alert
    bool alert_clear;        // the session moved on: end the alert
    bool enterprise;         // true = Enterprise spending-limit account
    int time_pct;            // 0-100: fraction of billing period elapsed (Enterprise)
    int period_days;         // total billing period length in days (Enterprise)
    char reset_date[12];     // formatted reset date e.g. "Jul 1" (Enterprise)
    long clock_epoch;        // local wall-clock epoch (s) from the app; 0 = not provided
    int  clock_fmt;          // 12 or 24 (hour format from the app); defaults to 24
    // ---- Fast clock (Claude Code hooks via the app). All optional: an older
    // app sends none of them and the fields read as "absent" below, which
    // the signal logic treats exactly like "no hooks installed".
    int8_t   activity;       // `a`  0-3 tool-call activity level; -1 = absent
    char     event;          // `e`  latest hook event: 'p' prompt, 's' stop,
                             //      'b' back after a gap; '\0' = absent. Other
                             //      chars pass through for forward compat.
    bool     has_event_seq;  // `es` present
    uint32_t event_seq;      // `es` sequence number; react once per change
    int16_t  sessions;       // `n`  sessions with a hook event in the last 5 min; -1 = absent
    bool     replay;         // `rp` 1 = heartbeat replay of the last payload;
                             //      the rate math and reset detection skip it
    bool ok;                 // data parse succeeded
    bool valid;              // false until first successful parse
};
