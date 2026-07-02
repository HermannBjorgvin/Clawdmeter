#include "../../hal/power_hal.h"
#include "board.h"
#include "fuel_gauge.h"
#include <Arduino.h>

// No PMU and no PWR button on this board (see board.h) — battery % and
// charge state come from the BQ27220 fuel gauge instead. is_vbus_in is
// approximated as "currently charging": the fuel gauge doesn't expose a
// distinct charger-present signal in this minimal driver, so a battery
// that's full while still plugged in will misreport as unplugged. Revisit
// once this is verified against real hardware.

#define BATTERY_POLL_MS 2000

static int      cached_pct      = -1;
static bool     cached_charging = false;
static uint32_t last_battery_ms = 0;

void power_hal_init(void) {
    if (!fuel_gauge_init()) return;
    cached_pct = fuel_gauge_read_pct();
    cached_charging = fuel_gauge_read_current_ma() > 10;
}

void power_hal_tick(void) {
    uint32_t now = millis();
    if (now - last_battery_ms >= BATTERY_POLL_MS) {
        last_battery_ms = now;
        cached_pct = fuel_gauge_read_pct();
        cached_charging = fuel_gauge_read_current_ma() > 10;
    }
}

int  power_hal_battery_pct(void) { return cached_pct; }
bool power_hal_is_charging(void) { return cached_charging; }
bool power_hal_is_vbus_in(void)  { return cached_charging; }

// No PWR button on this board's stock hardware — splash-cycling,
// brightness-cycling, and the hold-to-pair gesture are unavailable here.
bool power_hal_pwr_pressed(void)      { return false; }
bool power_hal_pwr_long_pressed(void) { return false; }
bool power_hal_pwr_released(void)     { return false; }
