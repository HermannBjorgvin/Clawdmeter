#include "../../hal/power_hal.h"
#include <Arduino.h>

// No PMU, no battery, no dedicated power button on this board.
// IDLE_SLEEP_WHEN_CHARGING=false in idle_cfg.h means idle.cpp will call
// power_hal_is_vbus_in() to keep the display on while USB is connected.
// Return true unconditionally here so the device never auto-sleeps while
// plugged in — it has no way to detect VBUS without a PMU.

void power_hal_init(void) {}
void power_hal_tick(void) {}

int  power_hal_battery_pct(void)  { return -1; }
bool power_hal_is_charging(void)  { return false; }
bool power_hal_is_vbus_in(void)   { return true; }   // always-on, no PMU
bool power_hal_pwr_pressed(void)       { return false; }
bool power_hal_pwr_long_pressed(void)  { return false; }
bool power_hal_pwr_released(void)      { return false; }
