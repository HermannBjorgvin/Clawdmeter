#pragma once
#include <Arduino.h>

// ---- Claude Code session list ----------------------------------------------
//
// Arrives on its own GATT characteristic (SS_CHAR), not the usage write: the
// usage payload already reaches ~121 bytes worst case, which would leave only
// ~79 bytes inside the ~200-byte MTU budget — about two rows.
//
// Fed by the daemon's hook listener (daemon/hook_listener.py), which receives
// Claude Code `"type": "http"` hook events. The daemon holds far richer state
// per session than these rows carry, so extending this struct never means
// touching the daemon's ingest path.

// 20 visible chars + NUL, matching BLE_LABEL_CHARS. 20 covers every real project
// name seen (longest: "data-pipeline-svc") and fits the measured 253-byte usable
// write. The daemon may send fewer on a link that negotiates a smaller MTU, and
// marks any truncation with a trailing "..." inside this budget.
#define SESSION_LABEL_LEN 21
#define MAX_SESSION_ROWS  5           // matches BLE_MAX_ROWS

// Wire codes — append only. These cross the BLE boundary, so renumbering would
// desync any daemon/firmware pair of different vintages. Mirrors
// BLE_STATE_CODES in daemon/hook_listener.py.
enum session_state_t : uint8_t {
    SESS_STARTING        = 0,
    SESS_IDLE            = 1,   // finished a turn, waiting for you to type
    SESS_THINKING        = 2,
    SESS_RESPONDING      = 3,
    SESS_RUNNING_TOOL    = 4,
    SESS_COMPACTING      = 5,
    SESS_WAIT_PERMISSION = 6,
    SESS_WAIT_QUESTION   = 7,
    SESS_WAIT_INPUT      = 8,
    SESS_ERROR           = 9,
    SESS_ENDED           = 10,
    SESS_STATE_COUNT,
};

// Mirrors BLE_MODEL_CODES in daemon/hook_listener.py. 0 = unknown.
enum session_model_t : uint8_t {
    SESS_MODEL_UNKNOWN = 0,
    SESS_MODEL_OPUS    = 1,
    SESS_MODEL_SONNET  = 2,
    SESS_MODEL_HAIKU   = 3,
    SESS_MODEL_FABLE   = 4,
    SESS_MODEL_COUNT,
};

struct SessionRow {
    char     label[SESSION_LABEL_LEN];  // project name, already truncated by the daemon
    uint8_t  state;                     // session_state_t
    int8_t   ctx_pct;                   // 0..100 context used; -1 = unknown
    uint16_t elapsed_s;                 // seconds in the current state
    uint8_t  model;                     // session_model_t
};

struct SessionList {
    SessionRow rows[MAX_SESSION_ROWS];
    uint8_t count;   // rows actually populated (<= MAX_SESSION_ROWS)
    uint8_t total;   // TRUE number running; total - count = "N more running"
    bool    valid;   // false until the first successful parse
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
    bool ok;                 // data parse succeeded
    bool valid;              // false until first successful parse
};
