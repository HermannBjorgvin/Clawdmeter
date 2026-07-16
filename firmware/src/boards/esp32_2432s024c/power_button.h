#pragma once

#include <stdint.h>

struct PowerButtonState {
    bool down = false;
    bool long_sent = false;
    uint32_t down_since_ms = 0;
};

struct PowerButtonEvents {
    bool pressed = false;
    bool long_pressed = false;
    bool released = false;
};

inline PowerButtonEvents update_power_button(
    PowerButtonState& state,
    bool down,
    uint32_t now_ms
) {
    PowerButtonEvents events{};

    if (down && !state.down) {
        state.down = true;
        state.long_sent = false;
        state.down_since_ms = now_ms;
        return events;
    }

    if (down && !state.long_sent && now_ms - state.down_since_ms >= 1500) {
        state.long_sent = true;
        events.long_pressed = true;
        return events;
    }

    if (!down && state.down) {
        events.pressed = !state.long_sent;
        events.released = true;
        state.down = false;
    }

    return events;
}
