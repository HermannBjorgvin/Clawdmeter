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

    // Antigravity (Google's agent IDE) — monthly prompt + flow credits
    // pulled from the local language_server's GetUserStatus RPC.
    float antig_session_pct;       // prompt-credit % used
    int   antig_session_reset_mins;
    float antig_weekly_pct;        // flow-credit % used
    int   antig_weekly_reset_mins;
    char  antig_plan[16];          // short plan name, e.g. "Ultra" / "Pro"
    char  antig_prompt_count[24];  // pre-formatted "500/50K"
    char  antig_flow_count[24];    // pre-formatted "100/150K"
    bool  antig_valid;

    // Per-model quota lines — daemon pre-formats each entry as
    // "Model Name|status" e.g. "Claude Sonnet|ok" or "Gemini 3.5 H|47m".
    // Up to 8 models. Empty strings mark unused slots.
    char  antig_model_lines[8][32];
    uint8_t antig_model_count;
};
