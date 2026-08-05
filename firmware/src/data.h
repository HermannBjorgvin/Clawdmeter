#pragma once
#include <Arduino.h>

// Weekly scoped-model limits ("ws" payload key). Some plans meter specific
// models separately inside the weekly window (today: Fable). Labels come from
// the API so future scoped models ride along without a firmware change.
#define MAX_SCOPED_WEEKLY 4
struct ScopedWeekly {
    char name[16];           // model label from the daemon (e.g. "Fable")
    float pct;               // utilization 0-100 (0% is a real value)
};

struct UsageData {
    float session_pct;       // utilization 0-100 (5h window Pro/Max; spending % Enterprise)
    int session_reset_mins;  // minutes until reset
    float weekly_pct;        // 7-day utilization (Pro/Max only; 0 for Enterprise)
    int weekly_reset_mins;   // minutes until weekly reset (Pro/Max only)
    int scoped_weekly_count; // 0 = plan has no scoped weekly limits ("ws" absent)
    ScopedWeekly scoped_weekly[MAX_SCOPED_WEEKLY];  // share the weekly reset instant
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
};
