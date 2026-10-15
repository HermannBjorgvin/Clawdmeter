#include "../../hal/power_hal.h"
#include <Arduino.h>

// No AXP2101 and no power button. An IP5306 power-bank PMIC is fitted, so the
// battery and charging hardware exist, but nothing reads it — hence
// has_battery = false in caps.cpp and no telemetry here.
// IDLE_SLEEP_WHEN_CHARGING=false in idle_cfg.h means idle.cpp calls
// power_hal_is_vbus_in() to keep the display on while USB is connected;
// return true unconditionally so the device never auto-sleeps.

void power_hal_init(void) {}
void power_hal_tick(void) {}

int  power_hal_battery_pct(void)  { return -1; }
bool power_hal_is_charging(void)  { return false; }
bool power_hal_is_vbus_in(void)   { return true; }   // always-on; IP5306 unread
bool power_hal_pwr_pressed(void)       { return false; }
bool power_hal_pwr_long_pressed(void)  { return false; }
bool power_hal_pwr_released(void)      { return false; }
