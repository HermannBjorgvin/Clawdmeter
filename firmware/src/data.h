#pragma once
#include <Arduino.h>

#define MAX_SESSIONS 6

enum sess_state_t {
    SESS_IDLE = 0,
    SESS_WORKING,
    SESS_WAITING,
};

struct SessionInfo {
    char         proj[16];     // last component of cwd, truncated
    sess_state_t state;
    char         msg[48];      // only populated for waiting state
};

struct UsageData {
    float session_pct;       // 5-hour window utilization (0-100)
    int session_reset_mins;  // minutes until session resets
    float weekly_pct;        // 7-day window utilization (0-100)
    int weekly_reset_mins;   // minutes until weekly resets
    char status[16];         // "allowed" or "limited"
    bool ok;                 // data parse succeeded
    bool valid;              // false until first successful parse
    char attn_msg[96];       // non-empty → Claude needs the user; text is the prompt
    SessionInfo sessions[MAX_SESSIONS];
    int sessions_count;
};
