#pragma once
#include <stdint.h>
#include <stdio.h>

// Pure arithmetic behind the History screen. No LVGL/Arduino dependencies so
// it can be unit-tested on the host (see test/test_history_math/), in the
// same spirit as splash_geometry.h.
//
// The daemon sends per-day buckets oldest → newest with today last, plus the
// weekday of that last bucket (Mon=0 … Sun=6). Everything derived — the
// running total for the current week, today vs. the trailing average, and
// the "will I hit the limit before it resets" projection — lives here.

#define HIST_DAYS   14
#define HIST_MIX_N  3

// Index of the first bucket of "this week". Prefers the daemon's window
// start (hs: the day Anthropic's rolling 7-day limit opened — what the
// weekly % actually meters); falls back to the calendar week's Monday when
// hs is absent (-1: enterprise accounts, older daemons).
static inline int history_week_start(int n, int weekday_last, int window_start) {
    if (n <= 0) return 0;
    if (window_start >= 0 && window_start < n) return window_start;
    if (weekday_last < 0) weekday_last = 0;
    if (weekday_last > 6) weekday_last = 6;
    int start = n - 1 - weekday_last;        // Mon=0 → today, Sun=6 → 6 days back
    return start < 0 ? 0 : start;
}

// Running total from bucket `start` to today, written into cum[] aligned
// with the day buckets (entries before start are left at -1 = "no point").
// Returns the number of days in the week so far (1..n).
static inline int history_week_cumulative(const int16_t* out_k, int n,
                                          int start, int32_t* cum) {
    if (n <= 0) return 0;
    if (start < 0) start = 0;
    if (start > n - 1) start = n - 1;
    int week_len = n - start;
    int32_t run = 0;
    for (int i = 0; i < n; ++i) {
        if (i < start) { cum[i] = -1; continue; }
        run += out_k[i];
        cum[i] = run;
    }
    return week_len;
}

// Today's bucket and the mean of the 7 buckets before it (or however many
// exist). avg7 is 0 when there is no prior day at all.
static inline void history_today_vs_avg(const int16_t* out_k, int n,
                                        int32_t* today, int32_t* avg7) {
    *today = 0;
    *avg7 = 0;
    if (n <= 0) return;
    *today = out_k[n - 1];
    int cnt = (n - 1 < 7) ? (n - 1) : 7;
    if (cnt <= 0) return;
    int32_t sum = 0;
    for (int i = n - 1 - cnt; i < n - 1; ++i) sum += out_k[i];
    *avg7 = sum / cnt;
}

// Largest bucket — the bar chart's y-scale. Never below 1 so a flat week
// still divides cleanly.
static inline int32_t history_max(const int16_t* out_k, int n) {
    int32_t m = 1;
    for (int i = 0; i < n; ++i) if (out_k[i] > m) m = out_k[i];
    return m;
}

// "1.34M", "881k", "12k", "0" — tokens from a thousands count. Sized for the
// small fonts: never wider than five characters.
static inline void history_fmt_k(int32_t k, char* buf, size_t len) {
    if (k >= 1000)     snprintf(buf, len, "%ld.%ldM", (long)(k / 1000), (long)((k % 1000) / 100));
    else if (k > 0)    snprintf(buf, len, "%ldk", (long)k);
    else               snprintf(buf, len, "0");
}

// --- pace projection --------------------------------------------------------
//
// Given the live session numbers and the smoothed burn rate, will the 5-hour
// window fill before it resets? rate is in %/min; a negative rate means the
// tracker is still warming up.

enum PaceKind {
    PACE_UNKNOWN = 0,   // no rate yet (warm-up) or no reset time
    PACE_IDLE,          // burn is effectively zero
    PACE_OK,            // window resets before the limit is reached
    PACE_LIMIT,         // limit hits first
};

typedef struct {
    PaceKind kind;
    int      minutes;   // PACE_LIMIT: minutes until 100%
    int      pct;       // PACE_OK: projected % at reset
} PaceProjection;

static inline PaceProjection history_pace(float session_pct, int reset_mins,
                                          float rate_pct_per_min) {
    PaceProjection p = { PACE_UNKNOWN, 0, 0 };
    if (rate_pct_per_min < 0.0f || reset_mins < 0) return p;
    if (rate_pct_per_min < 0.02f) { p.kind = PACE_IDLE; return p; }
    float headroom = 100.0f - session_pct;
    if (headroom < 0.0f) headroom = 0.0f;
    float mins_to_limit = headroom / rate_pct_per_min;
    if (mins_to_limit <= (float)reset_mins) {
        p.kind = PACE_LIMIT;
        p.minutes = (int)(mins_to_limit + 0.5f);
    } else {
        p.kind = PACE_OK;
        float at_reset = session_pct + rate_pct_per_min * (float)reset_mins;
        p.pct = (int)(at_reset + 0.5f);
        if (p.pct > 100) p.pct = 100;
    }
    return p;
}
