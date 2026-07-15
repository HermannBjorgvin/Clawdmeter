#pragma once
#include <Arduino.h>

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
    // Codex (OpenAI) usage — strictly optional. The daemon omits these keys on any
    // failure, so codex_valid=false must always fall back to the Claude-only view.
    float codex_pct;         // Codex utilization 0-100; <0 = daemon sent no Codex data
    int  codex_reset_mins;   // minutes until the Codex window resets
    int  codex_window_mins;  // Codex window length in minutes (10080 = weekly)
    bool codex_valid;        // true once a payload carried Codex data
    // Plan labels, pre-formatted by the daemon (the device never parses tiers).
    char plan[24];           // e.g. "Claude Max 20x"; "" = daemon sent none
    char codex_plan[24];     // e.g. "Codex Plus"
    bool ok;                 // data parse succeeded
    bool valid;              // false until first successful parse
};
