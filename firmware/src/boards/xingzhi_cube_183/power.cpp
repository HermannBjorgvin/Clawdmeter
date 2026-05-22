#include "../../hal/power_hal.h"
#include "board.h"
#include <Arduino.h>

// No PMU IC. Battery percentage comes from an ADC voltage-divider on
// GPIO17; charging status is the CHRG pin on GPIO38 (HIGH = charging).
// The PWR/cycle-screens button is VOL_UP (GPIO39) — we own it here so
// the shared screen-cycling code keeps working without a real PMU IRQ.

#define BATTERY_POLL_MS  2000
#define CHARGING_POLL_MS 500
#define PWR_POLL_MS      30

static int      cached_pct       = -1;
static bool     cached_charging  = false;
static bool     pwr_pressed_flag = false;
static bool     last_pwr_low     = false;   // GPIO39 active-LOW, debounced via poll interval
static uint32_t last_battery_ms  = 0;
static uint32_t last_charging_ms = 0;
static uint32_t last_pwr_ms      = 0;

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
    pinMode(BTN_PWR_GPIO,  INPUT_PULLUP);
    analogReadResolution(12);
    // ADC2 is shared with WiFi PHY — readings during active WiFi can fail,
    // but Clawdmeter only uses BLE so ADC2 stays usable.

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
    if (now - last_pwr_ms >= PWR_POLL_MS) {
        last_pwr_ms = now;
        bool low_now = digitalRead(BTN_PWR_GPIO) == LOW;
        if (low_now && !last_pwr_low) {
            pwr_pressed_flag = true;
        }
        last_pwr_low = low_now;
    }
}

int  power_hal_battery_pct(void) { return cached_pct; }
bool power_hal_is_charging(void) { return cached_charging; }
// CHRG (GPIO38) only goes HIGH while the charger is actively pushing
// current — drops back to LOW once the battery is full, and is
// ambiguous when no battery is fitted. There's no dedicated VBUS-sense
// pin on this kit, so we report "on USB power" unconditionally: the
// Xingzhi is desk-bound and effectively always plugged in. This keeps
// IDLE_SLEEP_WHEN_CHARGING=false behaving sanely — the screen never
// blanks while USB is connected — at the cost of also not sleeping on
// battery-only runs (which this kit isn't really designed for).
bool power_hal_is_vbus_in(void)  { return true; }

bool power_hal_pwr_pressed(void) {
    if (pwr_pressed_flag) {
        pwr_pressed_flag = false;
        return true;
    }
    return false;
}
