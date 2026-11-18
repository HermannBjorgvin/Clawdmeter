#ifdef PET_ASCII
#include "pet_hooks.h"
#include "pet_buddy.h"
#include "pet_stats_core.h"
#include "../ui.h"
#include "../clawd_policy.h"
#include <Arduino.h>
#include <Preferences.h>
#include <string.h>

static PetStats  stats;
static PetEnergy energy;
static bool      level_up = false;
static bool      usage_full = false;

// The alert on screen, for the answer time.
static bool      alert_up = false;
static uint32_t  alert_since = 0;

// ─── Storage ─────────────────────────────────────────────────────────────────
// One blob in NVS, written on events only (an answer, an ignored alert, a
// level, a nap worth saving), never on a timer: flash sectors wear out.
static const char* const NVS_NS  = "pet";
static const char* const NVS_KEY = "stats";
#define SAVE_VERSION 1

struct Saved {
    uint8_t  version;
    uint8_t  vel_idx;
    uint8_t  vel_count;
    uint8_t  pad;
    uint16_t answered;
    uint16_t ignored;
    uint32_t nap_seconds;
    uint32_t points_x100;
    uint16_t velocity[8];
};

static uint32_t unsaved_nap = 0;       // nap seconds counted since the last save

static void save(void) {
    Saved s = {};
    s.version = SAVE_VERSION;
    s.vel_idx = stats.vel_idx;
    s.vel_count = stats.vel_count;
    s.answered = stats.answered;
    s.ignored = stats.ignored;
    s.nap_seconds = stats.nap_seconds;
    s.points_x100 = stats.points_x100;
    memcpy(s.velocity, stats.velocity, sizeof(s.velocity));
    Preferences p;
    p.begin(NVS_NS, false);
    p.putBytes(NVS_KEY, &s, sizeof(s));
    p.end();
    unsaved_nap = 0;
}

void pet_hooks_init(void) {
    pet_stats_clear(&stats);
    pet_energy_boot(&energy, millis());
    Saved s = {};
    Preferences p;
    p.begin(NVS_NS, true);
    const size_t got = p.getBytes(NVS_KEY, &s, sizeof(s));
    p.end();
    if (got != sizeof(s) || s.version != SAVE_VERSION) return;
    stats.vel_idx = s.vel_idx % 8;
    stats.vel_count = s.vel_count > 8 ? 8 : s.vel_count;
    stats.answered = s.answered;
    stats.ignored = s.ignored;
    stats.nap_seconds = s.nap_seconds;
    stats.points_x100 = s.points_x100;
    memcpy(stats.velocity, s.velocity, sizeof(stats.velocity));
}

// ─── Inputs ──────────────────────────────────────────────────────────────────
void pet_on_payload(const UsageData* d, const SignalsUpdate* sig) {
    if (!d->valid || !d->ok) return;
    // A Codex account's numbers are not Claude's (signals reports them as a
    // replay), so they neither feed the pet nor make it dizzy.
    if (!d->codex) usage_full = d->session_pct >= 100.0f;
    // No 5-hour limit: the main meter is the week, whose rises weigh more.
    const uint8_t weight = d->weekly_only && !d->enterprise ? PET_WEEK_WEIGHT : 1;
    const uint8_t fed = pet_stats_fed(&stats);
    if (pet_stats_usage(&stats, d->session_pct, !sig->replay, sig->meter_kind_changed, weight)) {
        level_up = true;
        save();
    } else if (pet_stats_fed(&stats) != fed) {
        save();                        // a pip gained: keep it across a restart
    }
}

void pet_on_alert(void) {
    if (alert_up) return;              // a refresh keeps the first arrival time
    alert_up = true;
    alert_since = millis();
}

void pet_on_alert_cleared(void) {
    if (!alert_up) return;
    alert_up = false;
    pet_stats_answered(&stats, (uint32_t)(millis() - alert_since) / 1000u);
    save();
}

void pet_on_alert_dismissed(void) {
    if (!alert_up) return;
    alert_up = false;
    // The device ended it: by a tap (which says nothing about the answer), or
    // because it sat there for the whole alert timeout.
    if ((uint32_t)(millis() - alert_since) + 1000u >= clawd_tuning().alert_timeout_ms) {
        pet_stats_ignored(&stats);
        save();
    }
}

void pet_on_nap_end(uint32_t seconds) {
    if (seconds < 60) return;          // a blink of a rest is not a nap
    pet_stats_nap(&stats, seconds);
    pet_energy_refill(&energy, millis());
    // The screensaver dips back to the meters every few minutes, so naps end
    // often; save the total at most every ten minutes of napping.
    unsaved_nap += seconds;
    if (unsaved_nap >= 600) save();
}

// ─── Outputs ─────────────────────────────────────────────────────────────────
uint8_t pet_persona(void) {
    if (!ui_meters_live()) return PET_SLEEP;
    if (usage_full) return PET_DIZZY;
    if (signals_activity() > 0) return PET_BUSY;
    return PET_IDLE;
}

bool pet_take_level_up(void) {
    const bool r = level_up;
    level_up = false;
    return r;
}

uint16_t pet_level(void)  { return pet_stats_level(&stats); }
uint8_t  pet_fed(void)    { return pet_stats_fed(&stats); }
uint8_t  pet_mood(void)   { return pet_stats_mood(&stats); }
uint8_t  pet_energy(void) { return pet_energy_tier(&energy, millis()); }

#endif  // PET_ASCII
