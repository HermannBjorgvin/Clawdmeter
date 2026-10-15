#include "../../hal/power_hal.h"
#include "board.h"

#include <Arduino.h>

// No PMU, no battery, no PWR button — this board is USB-powered and has a
// single user button (BOOT). That button is wired here rather than to
// input_hal so the screen/brightness cycling and the hold-to-pair gesture stay
// reachable on a board whose resistive touch may need calibrating first.
//
// The three edges the shared loop consumes (short press, long-press crossing,
// release) are derived here from one debounced GPIO.

static const uint32_t DEBOUNCE_MS = 25;

static bool     down = false;
static bool     long_sent = false;
static uint32_t down_since_ms = 0;
static uint32_t last_edge_ms = 0;

static bool pressed_flag = false;
static bool long_pressed_flag = false;
static bool released_flag = false;

void power_hal_init(void) {
    pinMode(BTN_PWR_GPIO, INPUT_PULLUP);
    down = false;
    last_edge_ms = millis();
}

void power_hal_tick(void) {
    const uint32_t now = millis();
    const bool raw = digitalRead(BTN_PWR_GPIO) == LOW;

    if (raw != down) {
        if (now - last_edge_ms < DEBOUNCE_MS) return;
        last_edge_ms = now;
        down = raw;
        if (down) {
            long_sent = false;
            down_since_ms = now;
        } else {
            // A press that already fired its long-press edge must not also
            // fire a short press — the shared pair gesture is long + release.
            if (!long_sent) pressed_flag = true;
            released_flag = true;
        }
        return;
    }

    if (down && !long_sent && (now - down_since_ms) >= BTN_PWR_LONG_MS) {
        long_sent = true;
        long_pressed_flag = true;
    }
}

int  power_hal_battery_pct(void) { return -1; }     // no battery circuit
bool power_hal_is_charging(void) { return false; }
bool power_hal_is_vbus_in(void)  { return true; }   // USB is the only supply

bool power_hal_pwr_pressed(void) {
    const bool v = pressed_flag;
    pressed_flag = false;
    return v;
}

bool power_hal_pwr_long_pressed(void) {
    const bool v = long_pressed_flag;
    long_pressed_flag = false;
    return v;
}

bool power_hal_pwr_released(void) {
    const bool v = released_flag;
    released_flag = false;
    return v;
}
