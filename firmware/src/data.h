#pragma once
#include <Arduino.h>

// Max chats reported on the context screen; must match the daemon's
// CONTEXT_MAX_CHATS so page dots and payload size stay in sync.
#define CTX_MAX_CHATS 4

struct ChatCtx {
    char name[20];      // project name, pre-truncated by the daemon
    uint8_t pct;        // context window fill 0-100
    uint16_t used_k;    // tokens currently in context, thousands
    uint16_t limit_k;   // window size, thousands (200 or 1000)
    uint16_t age_min;   // minutes since this chat's last token activity ("a")
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
    ChatCtx chats[CTX_MAX_CHATS];  // context fill of active local chats ("cc")
    int chat_count;          // valid entries in chats[]; 0 = none reported
    bool ok;                 // data parse succeeded
    bool valid;              // false until first successful parse
};
