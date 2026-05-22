#include "../../hal/power_hal.h"
#include "board.h"
#include <Arduino.h>

// Minimal stub — replace with real power management for your board.
//
// If your board has an AXP2101 or similar PMU, mirror
// boards/waveshare_amoled_216/power.cpp. If the PWR button is wired
// somewhere other than the PMU's PKEY pin (e.g. through an IO expander
// like the AMOLED-1.8 board), look at that port instead.
//
// If your board has no PMU and no PWR button, leave the stubs as below
// and set BOARD_HAS_BATTERY=0 in board.h — the UI honors caps.has_battery
// and hides the battery indicator.

void power_hal_init(void) {}
void power_hal_tick(void) {}

int  power_hal_battery_pct(void) { return -1; }
bool power_hal_is_charging(void) { return false; }
bool power_hal_is_vbus_in(void)  { return false; }
bool power_hal_pwr_pressed(void) { return false; }
