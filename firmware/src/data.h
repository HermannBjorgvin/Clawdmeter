#pragma once
#include <Arduino.h>
#include <stdint.h>

struct UsageData {
    float session_pct;       // 5-hour window utilization (0-100)
    int session_reset_mins;  // minutes until session resets
    float weekly_pct;        // 7-day window utilization (0-100)
    int weekly_reset_mins;   // minutes until weekly resets
    char status[16];         // "allowed" or "limited"
    bool ok;                 // data parse succeeded
    bool valid;              // false until first successful parse
    char err[32];            // short error message from daemon (empty when ok)
    uint32_t last_msg_ms;    // millis() of last BLE message (any kind); 0 = never
};
