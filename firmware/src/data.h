#pragma once
#include <Arduino.h>

struct CodexData {
    float session_pct;       // primary window (5h) used % (0-100)
    int   session_reset_mins;
    float weekly_pct;        // secondary window (7d) used % (0-100)
    int   weekly_reset_mins;
    bool  valid;             // false until daemon sends cx_* fields
};

struct UsageData {
    float session_pct;       // 5-hour window utilization (0-100)
    int session_reset_mins;  // minutes until session resets
    float weekly_pct;        // 7-day window utilization (0-100)
    int weekly_reset_mins;   // minutes until weekly resets
    char status[16];         // "allowed" or "limited"
    bool ok;                 // data parse succeeded
    bool valid;              // false until first successful parse
    CodexData codex;         // Codex CLI usage (zero until cx_* fields arrive)
};
