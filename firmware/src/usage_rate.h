#pragma once

// Tracks short-term rate of change in session_pct (%/min) so the UI can react
// to *how heavily* Claude is being used right now, not just the current bucket
// level. Returns one of 4 group indices for the splash to pick animations from.

// Feed in the latest session percentage every time fresh BLE data arrives.
// Returns true when this sample is a session reset (pct dropped substantially
// vs the previous sample) — the caller uses this to chime the buzzer. Never
// true on the first sample after boot (no prior sample to compare against).
bool usage_rate_sample(float session_pct);

// 0 = idle, 1 = normal, 2 = active, 3 = heavy.
// Defaults to 0 when the buffer doesn't have enough samples yet.
// While a fresh running-agents count is known (see usage_rate_set_agents), the
// group follows the agents instead: none working = 0, then 1/2/3+ working = 1/2/3,
// lifted to the usage-rate group if that's higher.
int usage_rate_group(void);

// Feed the number of agents currently working (from the daemon's agents
// payload). Counts as known for AGENTS_STALE_MS, then the group falls back to
// the usage rate alone.
void usage_rate_set_agents(int working);
