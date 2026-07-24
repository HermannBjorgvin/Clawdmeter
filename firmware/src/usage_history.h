#pragma once
#include <Arduino.h>

// Longer-horizon usage history for the Trend and Burn-rate pages. Distinct from
// usage_rate.cpp (which keeps a tiny 6-sample ring purely to pick the splash
// animation intensity): this one stores both session% and weekly% over a wider
// window so the sparkline has something to draw and the burn-rate page can fit
// a real slope. Fed once per valid daemon update (~60s) from ui_update().
#define USAGE_HIST_SIZE 1440   // 24 hours of history at 60s daemon polling (Trend zoom-out max)

struct UsageHistPoint {
    uint32_t ms;      // millis() when sampled
    float    session; // 0-100
    float    weekly;  // 0-100
};

// Append a sample. epoch_now is the daemon's wall-clock (Unix seconds) at this
// update, or 0 if the clock feature is off — used only for persistence staleness,
// never for the in-RAM timeline (which stays millis-based). Persists to flash
// every few samples so the Trend survives reboots.
void usage_history_add(float session_pct, float weekly_pct, uint32_t epoch_now);

// Restore the saved history from flash into RAM. Call once at boot. The data is
// shown immediately; its age can't be judged until the daemon supplies a clock,
// so the first usage_history_add() with a valid epoch discards it if it's stale.
void usage_history_load(void);

// Freeze/unfreeze live sample intake. While frozen, usage_history_add() is a
// no-op so the sim demo's synthetic history stays on screen; the rest of the UI
// keeps updating from live data. usage_history_fill_sim() freezes automatically.
void usage_history_set_frozen(bool frozen);

// Clear the in-RAM history (flash copy untouched) — used on leaving sim mode so
// synthetic demo data is never persisted or mistaken for a real session reset.
void usage_history_reset(void);

// Clear RAM and wipe the persisted copy from flash.
void usage_history_clear_saved(void);

// Dev/demo: overwrite the buffer with a full 24h of synthetic multi-scale data
// (slow trend + mid oscillation + fine jitter) so the Trend page's zoom and
// Y-autoscale can be seen working without waiting hours for real 60s samples.
void usage_history_fill_sim(void);

// Number of stored samples (0..USAGE_HIST_SIZE).
int usage_history_count(void);

// Oldest-first access: i in [0, count). Returns nullptr if out of range.
const UsageHistPoint* usage_history_at(int i);

// Burn rate in %/hour over the most recent monotonic window (restarts at a
// detected reset). Returns false if there isn't yet enough history to trust it.
bool usage_history_session_rate(float* pct_per_hour);
bool usage_history_weekly_rate(float* pct_per_hour);
