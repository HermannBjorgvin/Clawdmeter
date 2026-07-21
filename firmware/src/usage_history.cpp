#include "usage_history.h"

// Same ~4-minute trust window as usage_rate.cpp: below this span a 1% blip
// between two 60s samples would read as a wild slope. Below it the rate helpers
// report "not ready" so the burn page shows a warm-up message instead of noise.
#define HIST_MIN_WINDOW_MS 240000UL

// A drop of more than this (percentage points) between consecutive samples is a
// window reset (5h session rollover or the weekly reset), not real burn — the
// rate window restarts there so a reset never shows as negative/again-climbing.
#define RESET_DROP_PP 5.0f

static UsageHistPoint ring[USAGE_HIST_SIZE];
static int count = 0;
static int head  = 0;  // next write slot

static inline int oldest_idx(void) {
    return (head + USAGE_HIST_SIZE - count) % USAGE_HIST_SIZE;
}

void usage_history_add(float session_pct, float weekly_pct) {
    ring[head].ms      = millis();
    ring[head].session = session_pct;
    ring[head].weekly  = weekly_pct;
    head = (head + 1) % USAGE_HIST_SIZE;
    if (count < USAGE_HIST_SIZE) count++;
}

int usage_history_count(void) {
    return count;
}

const UsageHistPoint* usage_history_at(int i) {
    if (i < 0 || i >= count) return nullptr;
    return &ring[(oldest_idx() + i) % USAGE_HIST_SIZE];
}

// Shared slope helper. `weekly` selects the series. Walks oldest→newest, moving
// the window start forward whenever a reset drop is seen, then fits a simple
// endpoint slope over the surviving span.
static bool rate_of(bool weekly, float* out_rate) {
    if (count < 2) return false;

    int start = 0;
    float prev = weekly ? usage_history_at(0)->weekly : usage_history_at(0)->session;
    for (int i = 1; i < count; i++) {
        const UsageHistPoint* p = usage_history_at(i);
        float v = weekly ? p->weekly : p->session;
        if (v + RESET_DROP_PP < prev) start = i;  // reset — restart the window here
        prev = v;
    }

    const UsageHistPoint* ps = usage_history_at(start);
    const UsageHistPoint* pn = usage_history_at(count - 1);
    uint32_t dt = pn->ms - ps->ms;
    if (dt < HIST_MIN_WINDOW_MS) return false;

    float dp = (weekly ? pn->weekly : pn->session) - (weekly ? ps->weekly : ps->session);
    if (dp < 0.0f) dp = 0.0f;
    *out_rate = dp * 3600000.0f / (float)dt;   // %/hour
    return true;
}

bool usage_history_session_rate(float* pct_per_hour) { return rate_of(false, pct_per_hour); }
bool usage_history_weekly_rate(float* pct_per_hour)  { return rate_of(true,  pct_per_hour); }
