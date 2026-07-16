#include <Arduino.h>
#include "idle.h"
#include "idle_cfg.h"
#include "hal/display_hal.h"
#include "hal/power_hal.h"

enum IdleState {
    STATE_AWAKE,
    STATE_FADING_OUT,
    STATE_ASLEEP,
    STATE_FADING_IN,
};

static IdleState state = STATE_AWAKE;
static uint32_t last_activity_ms = 0;
static uint32_t fade_started_ms  = 0;
static uint32_t fade_last_step_ms = 0;
static uint8_t  fade_from = DISPLAY_DEFAULT_BRIGHTNESS;
static uint8_t  fade_to   = 0;
static uint8_t  awake_brightness = DISPLAY_DEFAULT_BRIGHTNESS;  // user-set "full" level (brightness.cpp)

static void apply_brightness(uint8_t b) {
    display_hal_set_brightness(b);
}

static void begin_fade(uint8_t to, uint32_t now) {
    fade_from = (to == 0) ? awake_brightness : 0;
    fade_to   = to;
    fade_started_ms = now;
    fade_last_step_ms = now;
}

void idle_init(void) {
    state = STATE_AWAKE;
    last_activity_ms = millis();
    apply_brightness(awake_brightness);
}

void idle_set_awake_brightness(uint8_t level) {
    awake_brightness = level;
    // Apply now if fully awake so a button press is visible immediately;
    // during fades/sleep the next fade-in picks it up.
    if (state == STATE_AWAKE) apply_brightness(level);
}

static int claude_sessions = -1;   // <0 unknown / 0 idle / >0 working

void idle_set_claude_active(int sessions) {
    claude_sessions = sessions;
}

void idle_note_activity(void) {
    last_activity_ms = millis();
    if (state == STATE_FADING_IN) return;
    if (state == STATE_AWAKE) return;
    // Asleep/fading-out shouldn't reach here in normal flow (callers gate via
    // idle_consume_wake_press first), but if it does: trigger a wake.
    begin_fade(awake_brightness, last_activity_ms);
    state = STATE_FADING_IN;
}

bool idle_consume_wake_press(void) {
    if (state == STATE_ASLEEP || state == STATE_FADING_OUT) {
        uint32_t now = millis();
        last_activity_ms = now;
        begin_fade(awake_brightness, now);
        state = STATE_FADING_IN;
        return true;
    }
    if (state == STATE_FADING_IN) {
        // Mid-wake — still swallow this press; the user shouldn't get a
        // half-wake half-action surprise.
        last_activity_ms = millis();
        return true;
    }
    last_activity_ms = millis();
    return false;
}

bool idle_is_asleep(void) {
    return state == STATE_ASLEEP || state == STATE_FADING_OUT;
}

void idle_tick(void) {
    uint32_t now = millis();

    // Hold the panel awake (and light it back up): a working Claude session
    // counts as continuous activity, and so does USB power (if configured) —
    // but only while the activity state is unknown (old daemon / BLE down);
    // a known-idle Claude gets the short screen-off timer below even on USB.
    bool hold_awake = (claude_sessions > 0) ||
                      (!IDLE_SLEEP_WHEN_CHARGING && power_hal_is_vbus_in() &&
                       claude_sessions < 0);
    if (hold_awake) {
        last_activity_ms = now;
        if (state == STATE_ASLEEP || state == STATE_FADING_OUT) {
            begin_fade(awake_brightness, now);
            state = STATE_FADING_IN;
        }
    }

    switch (state) {
    case STATE_AWAKE: {
        uint32_t timeout = (claude_sessions >= 0) ? CLAUDE_IDLE_SLEEP_MS
                                                  : IDLE_TIMEOUT_MS;
        if (now - last_activity_ms >= timeout) {
            begin_fade(0, now);
            state = STATE_FADING_OUT;
        }
        break;
    }

    case STATE_FADING_OUT:
    case STATE_FADING_IN: {
        if (now - fade_last_step_ms < IDLE_FADE_STEP_MS) break;
        fade_last_step_ms = now;
        uint32_t dur = (state == STATE_FADING_OUT) ? IDLE_FADE_OUT_MS : IDLE_FADE_IN_MS;
        uint32_t elapsed = now - fade_started_ms;
        if (elapsed >= dur) {
            apply_brightness(fade_to);
            state = (state == STATE_FADING_OUT) ? STATE_ASLEEP : STATE_AWAKE;
        } else {
            // Linear interpolation fade_from -> fade_to over dur ms.
            int32_t span = (int32_t)fade_to - (int32_t)fade_from;
            int32_t b = (int32_t)fade_from + (span * (int32_t)elapsed) / (int32_t)dur;
            if (b < 0) b = 0;
            if (b > 255) b = 255;
            apply_brightness((uint8_t)b);
        }
        break;
    }

    case STATE_ASLEEP:
        break;
    }
}
