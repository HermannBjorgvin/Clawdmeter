#include "../../hal/power_hal.h"
#include "board.h"
#include <Arduino.h>

// No PMU, no battery gauge, no dedicated PWR button on this board.
// Battery/charging stubs return "unavailable" and the UI hides the indicator
// (BOARD_HAS_BATTERY=0).
//
// The one piece we can't just stub: the documented "hold PWR 3s, release ->
// pairing mode" gesture. With no PWR button, we synthesize the same three
// edge signals main.cpp expects from a long-press of the BOOT button, using
// the identical software hold-timer the 1.8 port runs off its EXIO4 line.
//
// Trade-off to be aware of: BOOT is also the primary PTT button (Space) read
// by input_hal. A short tap = PTT as normal; a deliberate >=3s hold will both
// hold Space for the duration AND trigger pairing on release. That only fires
// on an intentional 3s hold, so it's an acceptable wart. If you'd rather keep
// BOOT purely for PTT, drive the gesture from a touch long-press instead — but
// that needs a small edit to shared code (main.cpp), which a clean port avoids.

#define PWR_POLL_MS   50
#define PWR_LONG_MS   3000     // matches the documented 3-second hold

static bool     pwr_pressed_flag  = false;
static bool     pwr_long_flag     = false;
static bool     pwr_released_flag = false;
static bool     last_state        = false;
static uint32_t press_started_ms  = 0;
static bool     long_fired        = false;
static uint32_t last_poll_ms      = 0;

void power_hal_init(void) {
    // BOOT already set INPUT_PULLUP by input_hal_init(); harmless to repeat.
    pinMode(BTN_BACK_GPIO, INPUT_PULLUP);
}

void power_hal_tick(void) {
    uint32_t now = millis();
    if (now - last_poll_ms < PWR_POLL_MS) return;
    last_poll_ms = now;

    bool pressed_now = (digitalRead(BTN_BACK_GPIO) == LOW);   // active-low

    if (pressed_now && !last_state) {                 // rising edge — hold begins
        press_started_ms = now;
        long_fired = false;
    } else if (pressed_now && last_state) {           // held
        if (!long_fired && (now - press_started_ms >= PWR_LONG_MS)) {
            pwr_long_flag = true;
            long_fired = true;
        }
    } else if (!pressed_now && last_state) {          // falling edge — release
        pwr_released_flag = true;
        if (!long_fired) pwr_pressed_flag = true;     // treat as a short press
    }
    last_state = pressed_now;
}

int  power_hal_battery_pct(void) { return -1; }
bool power_hal_is_charging(void) { return false; }
bool power_hal_is_vbus_in(void)  { return false; }

bool power_hal_pwr_pressed(void) {
    if (pwr_pressed_flag) { pwr_pressed_flag = false; return true; }
    return false;
}
bool power_hal_pwr_long_pressed(void) {
    if (pwr_long_flag) { pwr_long_flag = false; return true; }
    return false;
}
bool power_hal_pwr_released(void) {
    if (pwr_released_flag) { pwr_released_flag = false; return true; }
    return false;
}
