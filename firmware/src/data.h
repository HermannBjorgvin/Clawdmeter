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
    // Not every provider meters both windows -- a Codex Pro plan has a weekly
    // quota and no 5-hour one. False means "this quota does not exist", which
    // the UI must render as blank rather than as a convincing 0%.
    bool has_session;
    bool has_weekly;
    // Non-empty when a panel is showing a specific model's quota rather than
    // the account's, e.g. "Spark". The pill says so instead of "Current".
    char session_model[13];
    char weekly_model[13];

    // Grants that restore a spent quota, where the provider offers them. When
    // a provider meters only one window the second card has nothing to show,
    // so it carries these instead of sitting empty.
    int  reset_credits;          // 0 = none / provider has no such thing
    int  reset_credits_exp_mins; // minutes until the soonest expiry ("rm"); -1 = unknown
    // Percent of the soonest-expiring credit's granted life still left ("rl"),
    // which the card draws as a bar filling toward that expiry. -1 = the
    // daemon could not read the detail endpoint, so the track stays empty.
    int  reset_credit_life;
    bool ok;                 // data parse succeeded
    bool valid;              // false until first successful parse
};
