#pragma once
#include <stdint.h>

// Usage rate → mood group ("slow clock"). Answers "how hard is Claude being
// used right now?" from the session % the app reports, so Clawd's mood can
// follow the last quarter hour instead of the bucket level.
//
// Model:
//   * 15-minute window over REAL samples only. Heartbeat replays (`rp`) are
//     dropped before they reach the ring.
//   * Rate is %/h of the 5-hour session, read as a share of pace (using the
//     window evenly = 20 %/h):
//         < 5 %/h   Idle      (0 or 1 point in 15 min)
//         < 10 %/h  Normal    (2 points)
//         < 20 %/h  Active    (3-4 points)
//         >= 20     Heavy     (5+ points: at pace or faster)
//   * The divisor is never shorter than the window, so a 1-point tick right
//     after boot reads 4 %/h, not 60. The baseline is the newest sample at
//     least one window older than `now`, so the rate decays to Idle on its own
//     once nothing has moved for 15 minutes, even with no new samples.
//   * Hook activity (`a`, 0-3) lifts the group: group = max(rate group,
//     activity). It never lowers it. A level expires USAGE_RATE_ACTIVITY_TTL_MS
//     after it was last set, so a dropped link can't pin Clawd at Heavy.
//   * Session reset = a real sample more than 5 points below the previous real
//     sample. The ring restarts from that sample.
//
// Every function taking `now_ms` is pure (no Arduino) and is what the native
// tests drive. The wrappers at the bottom read millis() and are what firmware
// code calls. Times are millis()-style uint32 and wrap-safe.

#define USAGE_RATE_WINDOW_MS        (15UL * 60UL * 1000UL)
#define USAGE_RATE_ACTIVITY_TTL_MS  (3UL * 60UL * 1000UL)

enum UsageRateGroup {
    RATE_IDLE   = 0,
    RATE_NORMAL = 1,
    RATE_ACTIVE = 2,
    RATE_HEAVY  = 3,
};

// Forget all samples and activity (boot state). Tests call it between cases.
void  usage_rate_clear(void);

// Feed one session % sample. `replay` samples are ignored entirely (return
// false). Returns true when this real sample is a session reset; never true on
// the first sample after boot/clear.
bool  usage_rate_sample_at(uint32_t now_ms, float session_pct, bool replay);

// Session %/h over the window ending at `now_ms` (0 with < 2 samples).
float usage_rate_pct_per_hour_at(uint32_t now_ms);

// Group from the rate alone (no activity lift).
int   usage_rate_base_group_at(uint32_t now_ms);

// Hook activity level 0-3 from the `a` field. level < 0 clears it.
void  usage_rate_set_activity_at(uint32_t now_ms, int level);

// Current activity level, or -1 when none was set or it expired.
int   usage_rate_activity_at(uint32_t now_ms);

// Mood group: max(base group, live activity level). 0..3, see UsageRateGroup.
int   usage_rate_group_at(uint32_t now_ms);

// ---- millis() wrappers (usage_rate_clock.cpp) ----
int   usage_rate_group(void);
int   usage_rate_base_group(void);    // rate only, no activity lift
