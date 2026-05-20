#pragma once
#include <Arduino.h>

struct UsageData {
    float session_pct;       // 5-hour window utilization (0-100)
    int session_reset_mins;  // minutes until session resets
    float weekly_pct;        // 7-day window utilization (0-100)
    int weekly_reset_mins;   // minutes until weekly resets
    char status[16];         // "allowed" or "limited"
    bool ok;                 // data parse succeeded
    bool valid;              // false until first successful parse
};

// Activity / TodoWrite mirror — populated from the BLE "sessions" array
// written by the daemon. Sized so the worst-case (3 sessions × 10 todos)
// fits in well under 4 KB of RAM.
#define MAX_SESSIONS            3
#define MAX_TODOS_PER_SESSION   10
#define TODO_CONTENT_LEN        64
#define TODO_ACTIVEFORM_LEN     56
#define PROJECT_NAME_LEN        28
#define MODEL_NAME_LEN          28
#define USER_PROMPT_LEN         80
#define CURRENT_TOOL_LEN        28
#define CURRENT_TOOL_ARGS_LEN   64

enum todo_status_t {
    TODO_PENDING     = 0,
    TODO_IN_PROGRESS = 1,
    TODO_COMPLETED   = 2,
};

// Per-session activity state. PHASE_RUNNING = a Stop event has not yet
// fired since the last tool/prompt event. PHASE_IDLE = the agent is
// resting (no current turn in flight).
enum phase_t {
    PHASE_IDLE    = 0,
    PHASE_RUNNING = 1,
};

struct TodoItem {
    char           content[TODO_CONTENT_LEN];
    char           active_form[TODO_ACTIVEFORM_LEN];   // empty unless in_progress
    todo_status_t  status;
};

struct SessionData {
    char      project[PROJECT_NAME_LEN];
    char      model[MODEL_NAME_LEN];
    char      last_prompt[USER_PROMPT_LEN];          // text of most recent UserPromptSubmit
    char      current_tool[CURRENT_TOOL_LEN];        // tool currently mid-call (PreToolUse → cleared on Stop)
    char      current_tool_args[CURRENT_TOOL_ARGS_LEN]; // short summary of tool_input (Bash command, file path, etc)
    phase_t   phase;
    uint32_t  last_active_secs;       // seconds since the daemon last saw activity
    TodoItem  todos[MAX_TODOS_PER_SESSION];
    uint8_t   todo_count;
};

struct ActivityData {
    SessionData  sessions[MAX_SESSIONS];
    uint8_t      session_count;
    bool         valid;               // false until first successful parse
};
