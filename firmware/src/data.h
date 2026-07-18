#pragma once
#include <Arduino.h>

// Attention event types — one numbering shared by the BLE protocol ("n"
// field), the UI views and the alert melodies. INPUT..CAL and CLEAR come
// from the daemon (as strings — renumbering is wire-safe); LIMIT and RESET
// are raised by the firmware itself.
enum : uint8_t {
    ATTN_NONE  = 0,
    ATTN_INPUT = 1,   // Claude is waiting for an answer
    ATTN_PERM  = 2,   // permission prompt
    ATTN_DONE  = 3,   // turn finished
    ATTN_CAL   = 4,   // upcoming meeting (daemon-side calendar reminder)
    ATTN_LIMIT = 5,   // session limit almost exhausted (firmware-local)
    ATTN_RESET = 6,   // session window reset (firmware-local)
    ATTN_CLEAR = 7,   // dismiss the attention view, no sound
};

struct UsageData {
    float session_pct;       // utilization 0-100 (5h window Pro/Max; spending % Enterprise)
    int session_reset_mins;  // minutes until reset
    char session_reset_at[12]; // wall-clock reset time, "21:00" / "ср 19:00"; "" unknown
    float weekly_pct;        // 7-day utilization (Pro/Max only; 0 for Enterprise)
    int weekly_reset_mins;   // minutes until weekly reset (Pro/Max only)
    char weekly_reset_at[12];  // wall-clock weekly reset; "" unknown
    char status[16];         // "allowed", "limited", etc.
    bool chime;              // play the session-reset chime; false unless daemon opts in
    uint8_t notify_type;     // hook-driven event: one of the ATTN_* values above
    char notify_project[97]; // event context line: git root basename, or
                             // "HH:MM title" for ATTN_CAL. 48 chars ≈ 96
                             // UTF-8 bytes when Cyrillic.
    int active_sessions;     // Claude Code sessions working right now; -1 = daemon doesn't report
    bool enterprise;         // true = Enterprise spending-limit account
    int time_pct;            // 0-100: fraction of billing period elapsed (Enterprise)
    int period_days;         // total billing period length in days (Enterprise)
    char reset_date[12];     // formatted reset date e.g. "Jul 1" (Enterprise)
    long clock_epoch;        // local wall-clock epoch (s) from daemon; 0 = not provided
    int  clock_fmt;          // 12 or 24 (hour format from daemon); defaults to 24
    bool ok;                 // data parse succeeded
    char err[8];             // why the daemon has no data: auth/rate/net/http/token; "" if ok
    bool valid;              // false until first successful parse
};
