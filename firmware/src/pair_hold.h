#pragma once
// Touch-hold re-pair for boards without a PWR-role button (BoardCaps
// has_pwr_button false, e.g. LCD-4, whose KEY/PWR is a hardware reset).
// While the pairing hint is on screen, holding a finger on the panel for
// PAIR_HOLD_MS clears the Bluetooth bonds so another computer can pair.
//
// Pure (no Arduino / LVGL) so the native tests cover it:
//   pio test -d firmware -e native -f test_pair_hold
#include <stdint.h>

#define PAIR_HOLD_MS     5000UL   // finger down this long → clear bonds
#define PAIR_HOLD_HINT_MS 1000UL  // after this, the hint says "keep holding"

enum PairHoldEvent {
    PAIR_HOLD_NONE,
    PAIR_HOLD_HOLDING,   // held past PAIR_HOLD_HINT_MS (once per hold)
    PAIR_HOLD_FIRE,      // held past PAIR_HOLD_MS: clear the bonds (once per hold)
    PAIR_HOLD_CANCEL,    // a hold that had reached HOLDING ended before firing
};

enum PairHoldState {
    PAIR_HOLD_UP,        // no finger on the panel
    PAIR_HOLD_COUNTING,  // a hold that started while enabled
    PAIR_HOLD_SPENT,     // fired, cancelled or ineligible: wait for the lift
};

struct PairHold {
    PairHoldState state;
    bool     hinted;     // HOLDING was reported for this hold
    uint32_t down_at;
};

void pair_hold_clear(PairHold* h);

// One step per loop. `enabled`: the gesture applies right now (no PWR
// button, hint visible, panel awake). `pressed`: a finger is on the panel.
// A hold only counts when it starts while enabled; losing `enabled` midway
// cancels it. Lifting the finger ends the hold; it never fires on release.
PairHoldEvent pair_hold_step(PairHold* h, bool enabled, bool pressed, uint32_t now_ms);
