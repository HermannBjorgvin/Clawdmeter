#include "../../hal/power_hal.h"
#include "board.h"
#include <Arduino.h>

// No PMU. Battery percentage comes from the ADC divider on BAT_ADC_GPIO and
// charging from the CHRG pin (HIGH = charging). The MID button (GPIO39, active LOW) is
// the PWR-role input: brightness, hold ~1.5 s and
// release to pair. The edges are synthesized in software:
//   short    - fired on release if the hold was shorter than PWR_LONG_MS
//   long     - fired once when a hold crosses PWR_LONG_MS
//   release  - fired on every release edge

#define BATTERY_POLL_MS  2000
#define CHARGING_POLL_MS 500
#define PWR_POLL_MS 50
#define PWR_LONG_MS 1500

static bool     pwr_pressed_flag  = false;
static bool     pwr_long_flag     = false;
static bool     pwr_released_flag = false;
static bool     last_pwr_state    = false;
static bool     pwr_long_fired    = false;
static uint32_t pwr_press_started_ms = 0;
static uint32_t last_pwr_ms       = 0;
static int      cached_pct        = -1;
static bool     cached_charging   = false;
static uint32_t last_battery_ms   = 0;
static uint32_t last_charging_ms  = 0;

static int read_battery_pct_once(void) {
    int raw = analogRead(BAT_ADC_GPIO);
    int pct = (int)((float)(raw - BAT_ADC_RAW_EMPTY) * 100.0f /
                    (float)(BAT_ADC_RAW_FULL - BAT_ADC_RAW_EMPTY));
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}

void power_hal_init(void) {
    pinMode(BAT_CHRG_GPIO, INPUT);
    pinMode(BTN_PWR_GPIO, INPUT_PULLUP);
    analogReadResolution(12);
    cached_charging = digitalRead(BAT_CHRG_GPIO) == HIGH;
    cached_pct = read_battery_pct_once();
}

void power_hal_tick(void) {
    uint32_t now = millis();
    if (now - last_charging_ms >= CHARGING_POLL_MS) {
        last_charging_ms = now;
        cached_charging = digitalRead(BAT_CHRG_GPIO) == HIGH;
    }
    if (now - last_battery_ms >= BATTERY_POLL_MS) {
        last_battery_ms = now;
        cached_pct = read_battery_pct_once();
    }
    if (now - last_pwr_ms < PWR_POLL_MS) return;
    last_pwr_ms = now;

    bool pwr_now = (digitalRead(BTN_PWR_GPIO) == LOW);   // active LOW
    if (pwr_now && !last_pwr_state) {                    // press edge
        pwr_press_started_ms = now;
        pwr_long_fired = false;
    } else if (pwr_now && last_pwr_state) {              // held
        if (!pwr_long_fired && (now - pwr_press_started_ms >= PWR_LONG_MS)) {
            pwr_long_flag  = true;
            pwr_long_fired = true;
        }
    } else if (!pwr_now && last_pwr_state) {             // release edge
        pwr_released_flag = true;
        if (!pwr_long_fired) pwr_pressed_flag = true;    // short press
    }
    last_pwr_state = pwr_now;
}

int  power_hal_battery_pct(void) { return cached_pct; }
bool power_hal_is_charging(void) { return cached_charging; }
// CHRG only goes HIGH while the charger is pushing current (LOW once full, and
// ambiguous with no battery), and the kit has no VBUS-sense pin. Report USB as
// present: the board is desk-bound, and this keeps the screen from blanking on
// the idle-sleep path while plugged in.
bool power_hal_is_vbus_in(void)  { return true; }

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
