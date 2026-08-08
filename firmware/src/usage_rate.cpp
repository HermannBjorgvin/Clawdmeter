#include "usage_rate.h"
#include <Arduino.h>

// Thresholds in %/min. The API reports 5h utilisation to two decimal places of
// a fraction — "0.02" — so what arrives is whole percents and nothing finer
// exists to ask for. Every computed rate is therefore a multiple of
// 60000/window: with RING_SIZE 10 at ~61s polling that quantum is 0.109 %/min,
// and the only rates that can ever occur are 0, 0.109, 0.219, 0.328, 0.437…
//
// So the thresholds sit in the gaps between those levels rather than on round
// numbers. Placed on round numbers they land within 0.003 of a reachable
// level, which means the poll interval drifting by a second decides the band —
// the previous 0.10/0.20/0.33 put a one-percent rise at 0.197, three
// thousandths under the Normal ceiling, and made Active nearly unreachable
// (1.3% of the time against Heavy's 3.8%, measured over 67 hours of real logs).
//
// What each level means, which is what the bands are really chosen by:
//   0 %      →  Idle    nothing moved in the window
//   0.109    →  Normal  1% in ~9 min — 15h to fill the session
//   0.219    →  Active  2% — 7.6h
//   0.328    →  Heavy   3% — 5.1h, i.e. filling as fast as the window resets
//
// The Heavy boundary still lands on that 5-hour line; 0.273 is only where the
// comparison is written, and no achievable rate falls between 0.219 and 0.328.
#define RATE_THRESH_NORMAL  0.055f
#define RATE_THRESH_ACTIVE  0.164f
#define RATE_THRESH_HEAVY   0.273f

// Minimum span between oldest and newest sample before we trust the computed
// rate. The whole point of the ring buffer is to smooth out single-sample
// jitter — at 60s daemon polling, a 1% bump between two consecutive samples
// looks like 1 %/min (Heavy) but really just means you grew 1% in the last
// minute. We require ~4 min of accumulated history so the rate reflects a
// real trend, not one noisy delta. Side-effect: ~4 min warm-up after boot
// during which we report Idle.
#define MIN_WINDOW_MS       240000UL

// Ten samples spans ~9 minutes at 61s polling. Six spanned five, and five
// minutes of whole-percent data is too coarse to tell the middle bands apart:
// one percent of movement was 0.197 %/min, which skipped Active entirely.
// Doubling the window halves the quantum, and the cost is reaction time — the
// reading is an average over the window, so it now lags by ~4.5 minutes rather
// than ~2.5.
#define RING_SIZE 10

struct Sample { uint32_t ms; float pct; };

static Sample ring[RING_SIZE];
static uint8_t count = 0;
static uint8_t head  = 0;  // index of next write slot

static inline uint8_t oldest_idx(void) {
    return (head + RING_SIZE - count) % RING_SIZE;
}

static void usage_rate_reset(void) {
    count = 0;
    head  = 0;
}

bool usage_rate_sample(float session_pct) {
    uint32_t now = millis();
    bool was_reset = false;

    if (count > 0) {
        uint8_t latest = (head + RING_SIZE - 1) % RING_SIZE;
        // Session reset: pct dropped substantially. Restart tracking.
        if (session_pct + 5.0f < ring[latest].pct) {
            usage_rate_reset();
            was_reset = true;
        }
    }

    ring[head] = { now, session_pct };
    head = (head + 1) % RING_SIZE;
    if (count < RING_SIZE) count++;

    return was_reset;
}

int usage_rate_group(void) {
    if (count < 2) return 0;

    uint8_t o = oldest_idx();
    uint8_t l = (head + RING_SIZE - 1) % RING_SIZE;
    uint32_t dt = ring[l].ms - ring[o].ms;
    if (dt < MIN_WINDOW_MS) return 0;

    float dp = ring[l].pct - ring[o].pct;
    if (dp < 0.0f) dp = 0.0f;
    float rate = dp * 60000.0f / (float)dt;

    if (rate < RATE_THRESH_NORMAL) return 0;
    if (rate < RATE_THRESH_ACTIVE) return 1;
    if (rate < RATE_THRESH_HEAVY)  return 2;
    return 3;
}
