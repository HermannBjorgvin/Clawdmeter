#pragma once
// The orientation change every rotating board does the same way, called from
// its display_hal_tick(): blank the panel, switch to the new orientation,
// force a full LVGL redraw, then ramp back to the user's brightness over
// ~100 ms so the change reads as deliberate.
//
// The value is the board's own orientation code (a quadrant on the AMOLED
// boards, the MADCTL rotation on the LCD 1.54). The board's flush and touch
// paths read rotation_transition_shown(), not the live setting: that only
// changes here, between lv_timer_handler() runs, so no strip is drawn in the
// new orientation over a screen still in the old one, and touch never runs
// ahead of the picture.
#include <Arduino.h>
#include <lvgl.h>
#include <stdint.h>
#include "brightness.h"
#include "hal/display_hal.h"

struct RotationTransition {
    volatile uint8_t shown;   // what the panel shows now
    uint8_t  ramp_step;       // 0 = idle, 1..4 = ramping
    uint32_t ramp_last;
};

// One panel per board, so one state; `inline` keeps it single across files.
inline RotationTransition rotation_transition_state = {0, 0, 0};

// The orientation the panel was set up in (display_hal_begin).
inline void rotation_transition_begin(uint8_t shown) { rotation_transition_state.shown = shown; }

inline uint8_t rotation_transition_shown(void) { return rotation_transition_state.shown; }

// `target` is what the panel should show; `apply` switches the panel to it
// (MADCTL), or nullptr where the flush path rotates in software.
inline void rotation_transition_tick(uint8_t target, void (*apply)(uint8_t)) {
    RotationTransition& t = rotation_transition_state;
    const uint32_t now = millis();
    if (target != t.shown) {
        display_hal_set_brightness(0);
        if (apply) apply(target);
        t.shown = target;
        lv_obj_invalidate(lv_screen_active());
        t.ramp_step = 1;
        t.ramp_last = now;
        return;
    }
    if (t.ramp_step == 0 || now - t.ramp_last < 25) return;
    t.ramp_last = now;
    // Back to the user's chosen brightness (not a fixed level), so a turn
    // doesn't undo what they set with a PWR short press.
    static const uint8_t pct[] = {30, 60, 85, 100};
    display_hal_set_brightness((uint8_t)(((uint16_t)brightness_get() * pct[t.ramp_step - 1]) / 100));
    t.ramp_step = t.ramp_step >= 4 ? 0 : t.ramp_step + 1;
}
