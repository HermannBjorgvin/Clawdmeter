#include "pet_stats_core.h"
#include <string.h>

void pet_stats_clear(PetStats* s) { memset(s, 0, sizeof(*s)); }

bool pet_stats_usage(PetStats* s, float pct, bool real, bool kind_changed, uint8_t weight) {
    if (!real) return false;
    if (pct < 0) pct = 0;
    if (kind_changed || !s->have_pct || pct <= s->last_pct) {
        s->have_pct = true;
        s->last_pct = pct;
        return false;
    }
    const uint16_t before = pet_stats_level(s);
    float rise = pct - s->last_pct;
    if (rise > 100) rise = 100;
    s->points_x100 += (uint32_t)(rise * 100.0f + 0.5f) * (weight ? weight : 1);
    s->last_pct = pct;
    return pet_stats_level(s) > before;
}

void pet_stats_answered(PetStats* s, uint32_t seconds) {
    if (s->answered < UINT16_MAX) s->answered++;
    s->velocity[s->vel_idx] = (uint16_t)(seconds > 65535u ? 65535u : seconds);
    s->vel_idx = (uint8_t)((s->vel_idx + 1) % 8);
    if (s->vel_count < 8) s->vel_count++;
}

void pet_stats_ignored(PetStats* s) {
    if (s->ignored < UINT16_MAX) s->ignored++;
}

void pet_stats_nap(PetStats* s, uint32_t seconds) { s->nap_seconds += seconds; }

uint16_t pet_stats_level(const PetStats* s) {
    const uint32_t lvl = s->points_x100 / (PET_POINTS_PER_LEVEL * 100u);
    return (uint16_t)(lvl > UINT16_MAX ? UINT16_MAX : lvl);
}

uint8_t pet_stats_fed(const PetStats* s) {
    const uint32_t into = s->points_x100 % (PET_POINTS_PER_LEVEL * 100u);
    return (uint8_t)(into / (PET_POINTS_PER_LEVEL * 100u / PET_FED_PIPS));
}

uint16_t pet_stats_median_velocity(const PetStats* s) {
    if (s->vel_count == 0) return 0;
    uint16_t tmp[8];
    memcpy(tmp, s->velocity, sizeof(tmp));
    const uint8_t n = s->vel_count > 8 ? 8 : s->vel_count;
    for (uint8_t i = 1; i < n; i++) {      // insertion sort, n <= 8
        const uint16_t k = tmp[i];
        int8_t j = (int8_t)(i - 1);
        while (j >= 0 && tmp[j] > k) { tmp[j + 1] = tmp[j]; j--; }
        tmp[j + 1] = k;
    }
    return tmp[n / 2];
}

uint8_t pet_stats_mood(const PetStats* s) {
    const uint16_t vel = pet_stats_median_velocity(s);
    int tier;
    if (s->vel_count == 0) tier = 2;        // no data: neutral
    else if (vel < 15)     tier = 4;
    else if (vel < 30)     tier = 3;
    else if (vel < 60)     tier = 2;
    else if (vel < 120)    tier = 1;
    else                   tier = 0;
    const uint32_t a = s->answered, d = s->ignored;
    if (a + d >= 3) {                       // a few alerts before judging
        if (d > a)          tier -= 2;
        else if (d * 2 > a) tier -= 1;
    }
    if (tier < 0) tier = 0;
    return (uint8_t)tier;
}

void pet_energy_boot(PetEnergy* e, uint32_t now_ms) {
    e->since_ms = now_ms;
    e->at_since = 3;
}

void pet_energy_refill(PetEnergy* e, uint32_t now_ms) {
    e->since_ms = now_ms;
    e->at_since = PET_ENERGY_MAX;
}

uint8_t pet_energy_tier(const PetEnergy* e, uint32_t now_ms) {
    const uint32_t hours = (uint32_t)(now_ms - e->since_ms) / 3600000u;
    const int tier = (int)e->at_since - (int)(hours / 2);
    return (uint8_t)(tier < 0 ? 0 : tier > PET_ENERGY_MAX ? PET_ENERGY_MAX : tier);
}
