#pragma once
#include <Arduino.h>

struct UsageData {
    float session_pct;       // 5-hour window utilization (0-100)
    int session_reset_mins;  // minutes until session resets
    float weekly_pct;        // 7-day window utilization (0-100)
    int weekly_reset_mins;   // minutes until weekly resets
    char status[16];         // "allowed" or "limited"
    char clock[9];           // "HH:MM:SS" host wall-clock (from daemon), or "" if absent
    uint8_t day7[7];         // last 7 days usage, 0-100 normalized (index 6 = today)
    bool ok;                 // data parse succeeded
    bool valid;              // false until first successful parse
};
