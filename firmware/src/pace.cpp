#include "pace.h"

static const float PACE_GRACE_PCT = 1.0f;

float pace_elapsed_pct(int reset_mins, int window_mins) {
    if (reset_mins < 0 || window_mins <= 0) return -1.0f;
    // Host clock skew or rounding can put the countdown past the window length.
    if (reset_mins > window_mins) reset_mins = window_mins;
    return 100.0f * (float)(window_mins - reset_mins) / (float)window_mins;
}

PaceLevel pace_level(float used_pct, float elapsed_pct) {
    if (elapsed_pct < 0) return PACE_UNKNOWN;
    if (used_pct >= 100.0f) return PACE_LIMIT;
    return used_pct > elapsed_pct + PACE_GRACE_PCT ? PACE_AHEAD : PACE_ON;
}
