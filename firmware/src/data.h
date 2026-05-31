#pragma once
#include <Arduino.h>

// Max accounts the device will display. Bounded by the BLE buffer
// (BLE_BUF_SIZE=512, ~60 JSON bytes/account) and the daemon, which sends a
// {"a":[...]} array. Keep in sync with the daemon's expectations.
#define MAX_ACCOUNTS 8

struct UsageData {
    char name[16];           // account label (empty for legacy single-account)
    float session_pct;       // 5-hour window utilization (0-100)
    int session_reset_mins;  // minutes until session resets
    float weekly_pct;        // 7-day window utilization (0-100)
    int weekly_reset_mins;   // minutes until weekly resets
    char status[16];         // "allowed" or "limited"
    bool ok;                 // this account polled successfully
    bool valid;              // false until first successful parse
};
