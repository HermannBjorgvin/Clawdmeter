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
    // OpenCode Go — same window model as Anthropic's (rolling/weekly percent +
    // reset), from the gateway's /usage endpoint. Absent from the payload on
    // hosts without OpenCode → oc_valid false → the screen reads "No data".
    bool oc_valid;
    int  oc_rolling_pct;     // rolling window utilization 0-100
    int  oc_rolling_mins;    // minutes until the rolling window resets; -1 unknown
    int  oc_weekly_pct;      // weekly window utilization 0-100
    int  oc_weekly_mins;     // minutes until the weekly window resets; -1 unknown
    int  oc_monthly_pct;     // monthly window utilization 0-100
    char oc_status[16];      // "ok", etc.
    long oc_tokens;          // total tokens today (in + out + reasoning + cache)
    char oc_model[20];       // busiest model today, e.g. "deepseek-v4-flash"
    bool ok;                 // data parse succeeded
    bool valid;              // false until first successful parse
};
