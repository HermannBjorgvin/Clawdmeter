#pragma once

// Tracks short-term rate of change in session_pct (%/min) so the UI can react
// to *how heavily* Claude is being used right now, not just the current bucket
// level. Returns one of 4 group indices for the splash to pick animations from.

// Feed in the latest session percentage every time fresh BLE data arrives.
void usage_rate_sample(float session_pct);

// 0 = idle, 1 = normal, 2 = active, 3 = heavy.
// Defaults to 0 when the buffer doesn't have enough samples yet.
int usage_rate_group(void);

// Clear the buffer (e.g. when the 5-hour session window resets).
void usage_rate_reset(void);
