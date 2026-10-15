#include "pair_hold.h"

void pair_hold_clear(PairHold* h) {
    h->state = PAIR_HOLD_UP;
    h->hinted = false;
    h->down_at = 0;
}

PairHoldEvent pair_hold_step(PairHold* h, bool enabled, bool pressed, uint32_t now_ms) {
    if (!pressed) {
        const bool cancelled = h->state == PAIR_HOLD_COUNTING && h->hinted;
        pair_hold_clear(h);
        return cancelled ? PAIR_HOLD_CANCEL : PAIR_HOLD_NONE;
    }
    switch (h->state) {
    case PAIR_HOLD_UP:
        // Only a press that begins while the gesture applies counts.
        h->state = enabled ? PAIR_HOLD_COUNTING : PAIR_HOLD_SPENT;
        h->hinted = false;
        h->down_at = now_ms;
        return PAIR_HOLD_NONE;
    case PAIR_HOLD_COUNTING: {
        if (!enabled) {
            // The hint went away (connected, another room, asleep).
            h->state = PAIR_HOLD_SPENT;
            return h->hinted ? PAIR_HOLD_CANCEL : PAIR_HOLD_NONE;
        }
        const uint32_t held = now_ms - h->down_at;
        if (held >= PAIR_HOLD_MS) {
            h->state = PAIR_HOLD_SPENT;
            return PAIR_HOLD_FIRE;
        }
        if (!h->hinted && held >= PAIR_HOLD_HINT_MS) {
            h->hinted = true;
            return PAIR_HOLD_HOLDING;
        }
        return PAIR_HOLD_NONE;
    }
    case PAIR_HOLD_SPENT:
        break;
    }
    return PAIR_HOLD_NONE;
}
