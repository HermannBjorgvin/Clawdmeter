#pragma once
#include <Arduino.h>

struct UsageData {
    float session_pct;       // 5-hour window utilization (0-100)
    int session_reset_mins;  // minutes until session resets
    float weekly_pct;        // 7-day window utilization (0-100)
    int weekly_reset_mins;   // minutes until weekly resets
    float overage_pct;       // "Extra Usage" (overage) utilization (0-100); 0 outside overage
    int overage_reset_mins;  // minutes until the binding window resets (-1 if unknown)
    bool overage_in_use;     // overage-in-use: pro-max shows "Extra Usage" title when true
    char acct[8];            // account type: "pro-max" (5h/7d windows) or "ent" (usage-based)
    char status[16];         // "allowed", "overage", "unknown", ...
    bool chime;              // play the session-reset chime; false unless daemon opts in
    long clock_epoch;        // local wall-clock epoch (s) from daemon; 0 = not provided
    int  clock_fmt;          // 12 or 24 (hour format from daemon); defaults to 24
    bool ok;                 // data parse succeeded
    bool valid;              // false until first successful parse
};
