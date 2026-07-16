#pragma once
#include <Arduino.h>

enum DashboardTransport : uint8_t {
    DASHBOARD_TRANSPORT_NONE,
    DASHBOARD_TRANSPORT_USB,
    DASHBOARD_TRANSPORT_BLE,
};

struct CodexLimitData {
    float percent;
    int window_mins;
    int reset_mins;
};

struct CodexData {
    CodexLimitData limits[2];
    uint8_t limit_count;
    uint32_t tokens_today;
    char plan[12];
    bool valid;
};

struct ActivityData {
    int claude_open;
    int claude_busy;
    int claude_waiting;
    int codex_unread;
    bool claude_valid;
    bool codex_valid;
    long scanned_epoch;
    bool valid;
};

struct UsageData {
    float session_pct;       // utilization 0-100 (5h window Pro/Max; spending % Enterprise)
    int session_reset_mins;  // minutes until reset
    float weekly_pct;        // 7-day utilization (Pro/Max only; 0 for Enterprise)
    int weekly_reset_mins;   // minutes until weekly reset (Pro/Max only)
    char status[16];         // "allowed", "limited", etc.
    bool chime;              // play the session-reset chime; false unless daemon opts in
    bool enterprise;         // true = Enterprise spending-limit account
    int time_pct;            // 0-100: fraction of billing period elapsed (Enterprise)
    int period_days;         // total billing period length in days (Enterprise)
    char reset_date[12];     // formatted reset date e.g. "Jul 1" (Enterprise)
    long clock_epoch;        // local wall-clock epoch (s) from daemon; 0 = not provided
    int  clock_fmt;          // 12 or 24 (hour format from daemon); defaults to 24
    bool ok;                 // data parse succeeded
    bool valid;              // false until first successful parse
    CodexData codex;
    ActivityData activity;
    long updated_epoch;
    DashboardTransport transport;
};
