#include "usage_rate.h"

// Pure: no Arduino. See usage_rate.h for the model.

#define RATE_NORMAL_PCT_H   5.0f
#define RATE_ACTIVE_PCT_H  10.0f
#define RATE_HEAVY_PCT_H   20.0f
#define RESET_DROP_PCT      5.0f

// Samples closer than this to the newest entry update its % in place instead
// of taking a slot (a prompt poll and a Stop poll 10 s apart, a reconnect
// burst). That bounds ring density at 2/min, so RING_SIZE entries always
// cover more than one window.
#define MERGE_MS        30000UL
#define RING_SIZE       40

struct Sample { uint32_t ms; float pct; };

static Sample   ring[RING_SIZE];
static uint8_t  count = 0;
static uint8_t  head  = 0;          // next write slot

static int      act_level = -1;
static uint32_t act_ms    = 0;

static inline const Sample& at_age(uint8_t i) {   // 0 = newest
    return ring[(head + RING_SIZE - 1 - i) % RING_SIZE];
}

void usage_rate_clear(void) {
    count = 0;
    head  = 0;
    act_level = -1;
    act_ms = 0;
}

bool usage_rate_sample_at(uint32_t now_ms, float session_pct, bool replay) {
    if (replay) return false;

    bool was_reset = false;
    if (count > 0) {
        const Sample& newest = at_age(0);
        if (session_pct + RESET_DROP_PCT < newest.pct) {
            count = 0;              // restart tracking from the new window
            head  = 0;
            was_reset = true;
        } else if ((uint32_t)(now_ms - newest.ms) < MERGE_MS) {
            ring[(head + RING_SIZE - 1) % RING_SIZE].pct = session_pct;
            return false;
        }
    }

    ring[head] = { now_ms, session_pct };
    head = (head + 1) % RING_SIZE;
    if (count < RING_SIZE) count++;
    return was_reset;
}

float usage_rate_pct_per_hour_at(uint32_t now_ms) {
    if (count < 2) return 0.0f;

    // Baseline: newest sample at least a window old; else the oldest we have.
    const Sample* base = &at_age(count - 1);
    for (uint8_t i = 0; i < count; i++) {
        const Sample& s = at_age(i);
        if ((uint32_t)(now_ms - s.ms) >= USAGE_RATE_WINDOW_MS) { base = &s; break; }
    }

    uint32_t dt = now_ms - base->ms;
    if (dt < USAGE_RATE_WINDOW_MS) dt = USAGE_RATE_WINDOW_MS;
    float dp = at_age(0).pct - base->pct;
    if (dp <= 0.0f) return 0.0f;
    return dp * 3600000.0f / (float)dt;
}

int usage_rate_base_group_at(uint32_t now_ms) {
    const float r = usage_rate_pct_per_hour_at(now_ms);
    if (r < RATE_NORMAL_PCT_H) return RATE_IDLE;
    if (r < RATE_ACTIVE_PCT_H) return RATE_NORMAL;
    if (r < RATE_HEAVY_PCT_H)  return RATE_ACTIVE;
    return RATE_HEAVY;
}

void usage_rate_set_activity_at(uint32_t now_ms, int level) {
    if (level < 0) { act_level = -1; return; }
    act_level = level > 3 ? 3 : level;
    act_ms = now_ms;
}

int usage_rate_activity_at(uint32_t now_ms) {
    if (act_level < 0) return -1;
    if ((uint32_t)(now_ms - act_ms) >= USAGE_RATE_ACTIVITY_TTL_MS) return -1;
    return act_level;
}

int usage_rate_group_at(uint32_t now_ms) {
    const int base = usage_rate_base_group_at(now_ms);
    const int act  = usage_rate_activity_at(now_ms);
    return act > base ? act : base;
}
