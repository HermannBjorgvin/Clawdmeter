#pragma once
#include <Arduino.h>

struct UsageData {
    float session_pct;       // Claude 5-hour window utilization (0-100)
    int session_reset_mins;  // minutes until Claude session resets
    float weekly_pct;        // Claude 7-day window utilization (0-100)
    int weekly_reset_mins;   // minutes until Claude weekly resets
    char status[16];         // "allowed" or "limited"
    bool ok;                 // data parse succeeded
    bool valid;              // false until first successful parse

    // Codex (OpenAI / ChatGPT subscription) usage — optional. Daemon
    // populates these from x-codex-primary-*-percent / -reset-after-seconds
    // headers on chatgpt.com/backend-api/codex/responses. -1 = no data
    // (e.g. Codex CLI not installed or daemon doesn't have access).
    float codex_session_pct;
    int   codex_session_reset_mins;
    float codex_weekly_pct;
    int   codex_weekly_reset_mins;
    bool  codex_valid;        // true once first Codex payload received
};
