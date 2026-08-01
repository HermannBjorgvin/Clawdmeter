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
    bool ok;                 // data parse succeeded
    bool valid;              // false until first successful parse
};

// ---- Live session awareness (issue #135) ----
// Compile-time gate for the chat views. Set per-board in board.h AND as a
// -D build flag in platformio.ini (shared code can't see board.h — same
// mechanism as BOARD_HAS_PSRAM). Absent → the views aren't compiled.
#ifndef BOARD_HAS_SESSION_VIEWS
#define BOARD_HAS_SESSION_VIEWS 0
#endif

// Wire codes below are APPEND-ONLY: they cross the BLE boundary, so
// renumbering would desync any host/firmware pair of different vintages.

enum session_state_t : uint8_t {
    SESSION_STARTING           = 0,   // idle bucket
    SESSION_IDLE               = 1,   // idle bucket
    SESSION_THINKING           = 2,   // working bucket
    SESSION_RESPONDING         = 3,   // working bucket
    SESSION_RUNNING_TOOL       = 4,   // working bucket
    SESSION_COMPACTING         = 5,   // working bucket
    SESSION_WAITING_PERMISSION = 6,   // waiting bucket — accent + pulse
    SESSION_WAITING_QUESTION   = 7,   // waiting bucket — accent + pulse
    SESSION_WAITING_INPUT      = 8,   // waiting bucket — accent + pulse
    SESSION_ERROR              = 9,   // waiting bucket — accent + pulse
    SESSION_ENDED              = 10,  // never sent to the device
};

enum session_model_t : uint8_t {
    SESSION_MODEL_UNKNOWN = 0,
    SESSION_MODEL_OPUS    = 1,
    SESSION_MODEL_SONNET  = 2,
    SESSION_MODEL_HAIKU   = 3,
    SESSION_MODEL_FABLE   = 4,
};

enum session_tool_t : uint8_t {
    SESSION_TOOL_NONE      = 0,   // other / none
    SESSION_TOOL_BASH      = 1,
    SESSION_TOOL_READ      = 2,
    SESSION_TOOL_EDIT      = 3,
    SESSION_TOOL_WRITE     = 4,
    SESSION_TOOL_GREP      = 5,
    SESSION_TOOL_GLOB      = 6,
    SESSION_TOOL_TASK      = 7,
    SESSION_TOOL_WEBFETCH  = 8,
    SESSION_TOOL_WEBSEARCH = 9,
};

// The panel fits 3.5 cards; rows past this cap are parsed only to be dropped.
// The host sorts before it sends, so what's dropped is what matters least.
#define SESSION_MAX_ROWS  6
#define SESSION_LABEL_MAX 32     // host middle-elides to fit the MTU budget;
                                 // the UI ellipsizes to the card width itself

struct SessionRow {
    char    sid[3];                  // 2 hex chars + NUL, stable for the session's
                                     // life — keys the card identity / reorder slide
    char    label[SESSION_LABEL_MAX];
    uint8_t state;                   // session_state_t
    int8_t  ctx_pct;                 // context fill 0-100; -1 = unknown (bar hidden)
    int32_t elapsed_s;               // seconds in the current state
    uint8_t model;                   // session_model_t
    uint8_t tool;                    // session_tool_t (shown when state==RUNNING_TOOL)
    uint8_t ntools;                  // concurrent pending tools
    uint8_t nagents;                 // running subagents; badge hidden at 0
    uint8_t tdone;                   // todos done
    uint8_t ttotal;                  // todos total; badge hidden at 0
    int32_t tok;                     // context tokens used, in units of 1k
                                     // (190 = 190k, 1200 = 1.2M); -1 = unknown.
                                     // Wire index 11; absent (older host) → -1.
};

struct SessionList {
    uint8_t    count;                // rows in use — host pre-sorted, render in order
    SessionRow rows[SESSION_MAX_ROWS];
};
