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
    long codex_context_tokens;  // current Codex context token count
    int  codex_context_window;  // Codex model context window in tokens
    bool codex_context_valid;   // true once the daemon sent a context reading
    // Antigravity CLI Gemini-pool usage. Optional; the daemon persists the last
    // valid quota response while `agy` is not running.
    float antigravity_5h_pct;
    int   antigravity_5h_reset_mins;
    float antigravity_weekly_pct;
    int   antigravity_weekly_reset_mins;
    bool  antigravity_valid;
    // Host resource snapshot from the Linux BLE daemon.
    float cpu_pct;
    int   cpu_temp_c;
    float gpu_pct;
    int   gpu_temp_c;
    float ram_pct;
    bool  system_valid;
    // Plan labels, pre-formatted by the daemon (the device never parses tiers).
    char plan[24];           // e.g. "Claude Max 20x"; "" = daemon sent none
    char codex_plan[24];     // e.g. "Codex Plus"
    char antigravity_plan[24]; // "Gemini Models"
    bool ok;                 // data parse succeeded
    bool valid;              // false until first successful parse
};

// One provider's /stats-style figures. Arrives in its OWN payload (marked "sv"),
// not the usage one — usage plus both providers' stats would overflow the 512B
// RX buffer. Everything is pre-computed by the daemon; the device does no date
// math and never sees a raw timestamp.
struct StatsData {
    bool  valid;             // false until a stats payload for this provider lands
    float total_tokens_m;    // millions, non-cache input + output (comparable across providers)
    char  model[16];         // favourite model, display name e.g. "Opus 4.8"
    int   sessions;
    long  longest_secs;      // longest single session
    int   active_days;       // days with activity
    int   span_days;         // days since first session
    int   streak;            // current streak
    int   best_streak;
    char  last_active[12];   // e.g. "Jul 14"
    int   dune;              // total tokens / ~245k (the novel)
    char  heat[50];          // 49 chars '0'-'4', oldest->newest, + NUL
};
