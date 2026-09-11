#pragma once
#include <stdint.h>

// 24 hours of Claude usage-% history (session + weekly) in an NVS-backed
// ring buffer — same design as sensor_hist.{h,cpp} (96 slots x 15 min, mean-
// averaged, persisted on every roll), kept as its own small module rather
// than folding into sensor_hist: usage payloads arrive on a different
// cadence/source (BLE poll, ~60 s) than the on-device env sensor (~10 s
// local reads), and sensor_hist's sample() signature is already coupled to
// its specific 3 physical fields. A sibling module mirroring the pattern is
// less risky than generalizing a working one under time pressure — see
// sensor_hist.h for the full ring-buffer design rationale (time anchoring,
// NVS wear, etc.), which applies here unchanged.

#define UH_SLOTS      96            // 96 x 15 min = 24 h
#define UH_SLOT_SECS  (15 * 60)

enum uh_metric_t {
    UH_SESSION,   // 5 h window %
    UH_WEEKLY,    // 7 d window %
    UH_METRIC_COUNT,
};

void usage_hist_init(void);              // restore the ring from NVS
void usage_hist_tick(void);              // call from loop(); rolls + persists
void usage_hist_set_time(long epoch_utc);// daemon clock; re-anchors the ring

// Feed a reading (0-100). Averaged into the slot currently being filled,
// which is also the newest point of the trace.
void usage_hist_sample(float session_pct, float weekly_pct);

// Copy one metric oldest-first into out[0..n-1] (n should be UH_SLOTS).
// Slots with no reading come back as NAN. Returns the number of real samples.
int  usage_hist_series(uh_metric_t m, float* out, int n);

void usage_hist_clear(void);              // wipe RAM + NVS copy

// QA only (serial "uhistfill"): synthesize a plausible 24 h trace so the
// sparklines can be laid out and screenshotted without waiting a day for
// real data. Overwrites whatever is in the ring, NVS copy included.
void usage_hist_debug_fill(void);
