#pragma once
#include <stdint.h>
#include <stdbool.h>

// The pet's stats, as pure rules (no Arduino, no storage) so they are
// unit-tested on the host (test/test_pet_stats). Ported from
// claude-desktop-buddy's stats.h (MIT, see LICENSE-buddy) with new inputs,
// because this device hears different things:
//
//   fed / level   rises in the main usage meter (the buddy counted output
//                 tokens): PET_POINTS_PER_LEVEL percentage points a level
//   answered      alerts the computer cleared (the buddy: approvals)
//   ignored       alerts that ran into the device's timeout (the buddy: denials)
//   velocity      seconds from an alert arriving to its clear
//   nap, energy   time the screensaver is up (the buddy: face-down time)

#define PET_POINTS_PER_LEVEL 500    // five full sessions, or one full week (below)
// A weekly meter moves a fifth as far as five sessions do, so its rises
// count this many times: an account with no 5-hour limit gains a level per
// full week, a "fed" pip per 10 % of it.
#define PET_WEEK_WEIGHT      5
#define PET_FED_PIPS         10
#define PET_MOOD_MAX         4
#define PET_ENERGY_MAX       5

struct PetStats {
    // Saved.
    uint32_t nap_seconds;
    uint32_t points_x100;      // usage points eaten, in hundredths of a point
    uint16_t answered;
    uint16_t ignored;
    uint16_t velocity[8];      // ring: seconds to answer, last 8 alerts
    uint8_t  vel_idx;
    uint8_t  vel_count;
    // Not saved: the last real reading of the meter, to take rises from.
    bool     have_pct;
    float    last_pct;
};

void pet_stats_clear(PetStats* s);

// One usage reading. `real` = a fresh poll of Claude's numbers (not a replay,
// not a Codex account); `kind_changed` = the meter now measures something
// else (session, week, spend). Only a rise since the last real reading of the
// same kind is eaten; a drop is a reset and just becomes the new baseline.
// `weight` multiplies the rise (PET_WEEK_WEIGHT for a weekly meter, else 1).
// True when this reading took the pet up a level.
bool pet_stats_usage(PetStats* s, float pct, bool real, bool kind_changed, uint8_t weight = 1);

void pet_stats_answered(PetStats* s, uint32_t seconds);
void pet_stats_ignored(PetStats* s);
void pet_stats_nap(PetStats* s, uint32_t seconds);

uint16_t pet_stats_level(const PetStats* s);
uint8_t  pet_stats_fed(const PetStats* s);               // 0 .. PET_FED_PIPS - 1
uint16_t pet_stats_median_velocity(const PetStats* s);   // 0 = no data
// 0 .. PET_MOOD_MAX. Quick answers lift it; ignoring more than a third of
// the alerts drags it down.
uint8_t  pet_stats_mood(const PetStats* s);

// Energy: 3 of 5 at boot, full when a nap ends, one tier less every 2 hours.
struct PetEnergy {
    uint32_t since_ms;
    uint8_t  at_since;
};
void    pet_energy_boot(PetEnergy* e, uint32_t now_ms);
void    pet_energy_refill(PetEnergy* e, uint32_t now_ms);
uint8_t pet_energy_tier(const PetEnergy* e, uint32_t now_ms);   // 0 .. PET_ENERGY_MAX
