#pragma once
#include <Arduino.h>

struct UsageData {
    float session_pct;       // 5-hour window utilization (0-100)
    int session_reset_mins;  // minutes until session resets
    float weekly_pct;        // 7-day window utilization (0-100)
    int weekly_reset_mins;   // minutes until weekly resets
    char status[16];         // "allowed" or "limited"
    char model[12];          // active session model, short ("Sonnet"); "" if unknown
    int  ctx_pct;            // active session context-window usage %, -1 if unknown
    bool ok;                 // data parse succeeded
    bool valid;              // false until first successful parse
};

struct CopilotData {
    int premium_pct;          // 0-100, % of monthly premium requests USED, -1 if unavailable
    int premium_remaining;    // requests remaining this month, -1 if unavailable
    int premium_total;        // monthly entitlement, -1 if unavailable
    int premium_reset_mins;   // minutes until monthly quota reset, -1 if unknown
    char premium_reset_str[24]; // formatted reset date from daemon, e.g. "Jun 1"
    char plan[20];            // e.g. "Pro", "Business", "Enterprise", "unknown"
    bool enabled;             // Copilot seat is active
    bool valid;               // false until first successful parse
};

struct SysInfoData {
    int cpu_pct;        // 0-100, CPU utilization; -1 if unavailable
    float cpu_temp;     // °C; -1 if unavailable
    int ram_pct;        // 0-100, RAM used %; -1 if unavailable
    float ram_used_gb;  // RAM used (GB)
    float ram_total_gb; // RAM total (GB)
    int disk_pct;       // 0-100, disk used %; -1 if unavailable
    float disk_used_gb; // disk used (GB)
    float disk_total_gb; // disk total (GB)
    bool valid;         // false until first successful parse
};

struct VscodeData {
    int mem_mb;         // total VS Code process RSS in MB; -1 if unavailable
    int cpu_pct;        // total VS Code CPU %; -1 if unavailable
    int ext_count;      // number of extension host processes; -1 if unavailable
    int error_count;    // error/critical log lines in last 30 min; -1 if unavailable
    char last_error[32];// last error snippet (truncated); empty if none
    bool valid;         // false until first successful parse
};

struct CiData {
    char state[10];    // "pass" | "fail" | "running" | "none"
    char wf[20];       // workflow name
    char branch[24];   // branch the run/repo is on
    int  age_min;      // minutes since the run finished/started; -1 unknown
    int  review;       // open PRs awaiting my review
    int  changes;      // my PRs with failing checks or changes requested
    int  dirty;        // changed files in the working tree; -1 unknown
    int  ahead, behind;
    bool conflict;
    bool valid;
};

struct EnvData {
    long epoch;          // unix seconds (UTC) at the moment the daemon sent this
    int  tz_off_min;     // local UTC offset in minutes (DST already resolved host-side)
    int  temp_c;         // current temperature °C
    int  hi_c, lo_c;     // today's high / low °C
    int  wcode;          // WMO weather code (open-meteo); -1 if unavailable
    char loc[16];        // short location name, e.g. "Bucharest"
    bool has_weather;    // false → time only (weather fetch failed)
    bool valid;          // false until first successful parse
};

struct TodayData {
    int  active_min;   // active minutes today
    int  tok_k;         // tokens today, thousands
    int  usd;           // cost today, whole dollars, -1 if unknown
    int  commits;       // commits today
    int  cp_used;        // Copilot premium requests used today, -1 if unknown
    bool valid;          // false until first successful parse
};

struct AuroraData {
    long epoch;        // unix seconds (UTC) at the moment the daemon sent this
    int  pct;           // 0-100 local aurora visibility probability (NOAA OVATION), -1 if unavailable
    int  kp_x10;         // current 3h-bucket planetary Kp index * 10, -1 if unavailable
    int  kpmax_x10;      // max Kp forecast over the next 24h * 10, -1 if unavailable
    int  cloud_pct;      // 0-100 cloud cover at the same location, -1 if unknown
    bool night;          // false = daylight (aurora won't be visible regardless of pct)
    bool valid;          // false until first successful parse
};
