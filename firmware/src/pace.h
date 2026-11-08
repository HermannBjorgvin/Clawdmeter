#pragma once

// Pace marker on the Pro/Max usage bars (device setting `pm`). A marker sits
// at the share of the window already elapsed (5 hours / 7 days); the fill is
// green at or behind it, amber ahead of it, red once the limit is hit. Pure:
// no LVGL, so it runs in the native unit tests.

#define PACE_SESSION_MINS (5 * 60)
#define PACE_WEEK_MINS    (7 * 24 * 60)

// Share of a window already elapsed, 0..100: where usage would sit if spread
// evenly over the window. -1 when the reset time is unknown (a missing
// `sr`/`wr` parses to -1). A countdown of 0 (run out, or clamped by a
// heartbeat replay until the next poll) is a fully elapsed window, matching
// the "Resets in 0m" label.
float pace_elapsed_pct(int reset_mins, int window_mins);

enum PaceLevel { PACE_UNKNOWN, PACE_ON, PACE_AHEAD, PACE_LIMIT };

// How usage compares with the elapsed share. Usage arrives as whole percents
// while elapsed is fractional, so usage within a point of the marker (which
// visually touches it) still counts as on pace.
PaceLevel pace_level(float used_pct, float elapsed_pct);
