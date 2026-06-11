#pragma once
#include <Arduino.h>

struct UsageData {
    float session_pct;         // 5-hour window utilization (0-100)
    int session_reset_mins;    // minutes until session resets
    int credits_used_cents;    // monthly credits used (in currency cents, e.g. 3554 = 35.54)
    int credits_limit_cents;   // monthly credits limit (in currency cents, e.g. 15000 = 150.00)
    char credits_currency[8];  // "EUR" or "USD"
    char status[16];           // "allowed" or "limited"
    bool ok;                   // data parse succeeded
    bool valid;                // false until first successful parse
};
