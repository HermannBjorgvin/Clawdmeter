#pragma once
#include <Arduino.h>

// Longer-horizon usage history for the Trend and Burn-rate pages. Distinct from
// usage_rate.cpp (which keeps a tiny 6-sample ring purely to pick the splash
// animation intensity): this one stores both session% and weekly% over a wider
// window so the sparkline has something to draw and the burn-rate page can fit
// a real slope. Fed once per valid daemon update (~60s) from ui_update().
#define USAGE_HIST_SIZE 60   // ~60 min of history at 60s daemon polling

struct UsageHistPoint {
    uint32_t ms;      // millis() when sampled
    float    session; // 0-100
    float    weekly;  // 0-100
};

void usage_history_add(float session_pct, float weekly_pct);

// Number of stored samples (0..USAGE_HIST_SIZE).
int usage_history_count(void);

// Oldest-first access: i in [0, count). Returns nullptr if out of range.
const UsageHistPoint* usage_history_at(int i);

// Burn rate in %/hour over the most recent monotonic window (restarts at a
// detected reset). Returns false if there isn't yet enough history to trust it.
bool usage_history_session_rate(float* pct_per_hour);
bool usage_history_weekly_rate(float* pct_per_hour);
