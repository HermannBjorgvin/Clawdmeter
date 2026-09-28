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

// Running Claude Code sessions, sent by the daemon as its own payload:
//   {"ag":[["name","b",12,"Edit ui.cpp"],…],"n":5}
//   state: b = working, w = waiting, d = just finished (daemon holds it ~60s
//   after busy → idle), i = idle; 4th field (optional) = what it's
//   doing now (last tool call), empty/absent when idle
#define AGENTS_MAX 4
struct AgentInfo {
    char name[20];
    char state;              // 'b' | 'w' | 'd' | 'i'
    int  mins;               // minutes in the current state
    char act[20];            // activity hint, "" when none
};
struct AgentsData {
    AgentInfo list[AGENTS_MAX];
    int count;               // rows in list[]
    int total;               // all running sessions (may exceed count)
};
