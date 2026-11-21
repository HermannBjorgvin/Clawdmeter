#include "../../hal/power_hal.h"
#include "board.h"
#include "ft6336.h"
#include "pmu.h"
#include <Arduino.h>

// Battery and USB state from the power chip (pmu.cpp). The PWR role is touch
// button B (the zone touch.cpp latched), with the same edge synthesis as the FIRE's button B:
//   short   - on release, if held less than PWR_LONG_MS
//   long    - once, when a hold crosses PWR_LONG_MS (starts hold-to-pair)
//   release - every release
// The side button stays the power chip's own on/off switch.

#define BATTERY_POLL_MS  2000
#define PWR_POLL_MS      30
#define PWR_LONG_MS      1500

static int      cached_pct = -1;
static bool     cached_charging = false, cached_vbus = false;
static bool     pressed_flag = false, long_flag = false, released_flag = false;
static bool     last_state = false, long_fired = false;
static uint32_t press_ms = 0, last_batt_ms = 0, last_pwr_ms = 0;

static void sample(void) {
    cached_pct      = pmu_battery_pct();
    cached_charging = pmu_is_charging();
    cached_vbus     = pmu_is_vbus_in();
}

void power_hal_init(void) { sample(); }

void power_hal_tick(void) {
    uint32_t now = millis();
    if (now - last_batt_ms >= BATTERY_POLL_MS) {
        last_batt_ms = now;
        sample();
    }
    if (now - last_pwr_ms >= PWR_POLL_MS) {
        last_pwr_ms = now;
        bool down = ft6336_zone() == TZ_BTN_B;
        if (down && !last_state) {
            press_ms = now;
            long_fired = false;
        } else if (down && !long_fired && now - press_ms >= PWR_LONG_MS) {
            long_flag = true;
            long_fired = true;
        } else if (!down && last_state) {
            released_flag = true;
            if (!long_fired) pressed_flag = true;
        }
        last_state = down;
    }
}

int  power_hal_battery_pct(void) { return cached_pct; }
bool power_hal_is_charging(void) { return cached_charging; }
bool power_hal_is_vbus_in(void)  { return cached_vbus; }

bool power_hal_pwr_pressed(void)      { bool f = pressed_flag;  pressed_flag = false;  return f; }
bool power_hal_pwr_long_pressed(void) { bool f = long_flag;     long_flag = false;     return f; }
bool power_hal_pwr_released(void)     { bool f = released_flag; released_flag = false; return f; }
