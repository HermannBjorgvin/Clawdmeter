#pragma once
#include <Arduino.h>
#include "history_math.h"   // HIST_DAYS / HIST_MIX_N

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
    // History (daemon "h"/"ht"/"hw"/"hm"; absent on older daemons → hist_days 0)
    int16_t hist_out_k[HIST_DAYS];   // output tokens per local day, thousands, oldest → newest
    int16_t hist_turns[HIST_DAYS];   // assistant turns per day, same order
    int8_t  hist_days;               // buckets received (0 = no history in this payload)
    int8_t  hist_weekday;            // weekday of the last bucket, Mon=0 … Sun=6
    int8_t  hist_week_start;         // bucket index where the rolling 7-day window opened; -1 = unknown
    char    hist_mix_name[HIST_MIX_N][8];  // model family, e.g. "Opus"
    uint8_t hist_mix_pct[HIST_MIX_N];      // output-token share, trailing 7 days
    uint8_t hist_mix_n;
    bool ok;                 // data parse succeeded
    bool valid;              // false until first successful parse
};
