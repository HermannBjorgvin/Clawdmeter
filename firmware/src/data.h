#pragma once
#include <Arduino.h>

enum UsageProvider : uint8_t {
    USAGE_PROVIDER_CLAUDE = 0,
    USAGE_PROVIDER_CODEX = 1,
    USAGE_PROVIDER_COUNT = 2,
};

struct ProviderUsageData {
    float session_pct;       // 5-hour window utilization (0-100)
    int session_reset_mins;  // minutes until session resets
    float weekly_pct;        // 7-day window utilization (0-100)
    int weekly_reset_mins;   // minutes until weekly resets
    char status[24];         // "allowed", "limited", or provider-specific error
    bool ok;                 // data parse succeeded
    bool valid;              // false until this provider has data
};

struct UsageData {
    ProviderUsageData providers[USAGE_PROVIDER_COUNT];
    UsageProvider primary_provider;  // legacy top-level fields map here
    bool dual;                       // true when both providers were received
    bool valid;                      // false until first successful parse
};
