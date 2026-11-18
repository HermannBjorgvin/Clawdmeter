#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "../data.h"
#include "../signals.h"

// What the rest of the firmware tells the pet, and what the pet modules ask
// back. In a build without -DPET_ASCII the calls from main.cpp are empty
// inlines, so main.cpp needs no #ifdef.
#ifdef PET_ASCII

void pet_hooks_init(void);            // load the saved stats (once, at boot)

// main.cpp, per parsed payload (after signals_ingest).
void pet_on_payload(const UsageData* d, const SignalsUpdate* sig);
// main.cpp, the alert's life: it arrived (or was refreshed), the computer
// cleared it (you answered there), or the device ended it itself (a tap, or
// its timeout: an alert that ran its whole time counts as ignored).
void pet_on_alert(void);
void pet_on_alert_cleared(void);
void pet_on_alert_dismissed(void);

// pet_face.cpp: a screensaver rest of this many seconds just ended.
void pet_on_nap_end(uint32_t seconds);

// What the pet is doing when nothing else is asked of it: asleep without
// live data, dizzy at a used-up limit, busy while tools run, else idle.
uint8_t pet_persona(void);
bool    pet_take_level_up(void);      // true once per level gained

uint16_t pet_level(void);
uint8_t  pet_fed(void);               // 0 .. PET_FED_PIPS - 1
uint8_t  pet_mood(void);              // 0 .. PET_MOOD_MAX
uint8_t  pet_energy(void);            // 0 .. PET_ENERGY_MAX

#else

static inline void pet_on_payload(const UsageData*, const SignalsUpdate*) {}
static inline void pet_on_alert(void) {}
static inline void pet_on_alert_cleared(void) {}
static inline void pet_on_alert_dismissed(void) {}

#endif
