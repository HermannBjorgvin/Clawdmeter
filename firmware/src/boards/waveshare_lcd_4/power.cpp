#include "../../hal/power_hal.h"

// No PMU and no GPIO-wired PWR button — KEY/PWR is EN/RST (hardware reset).
// Battery gauge is not exposed over I2C. USB-powered desk use reports VBUS
// present so idle-sleep stays off while plugged in (IDLE_SLEEP_WHEN_CHARGING
// is false). Hold-to-pair is unavailable; re-pair from the host OS.

void power_hal_init(void) {}
void power_hal_tick(void) {}

int  power_hal_battery_pct(void) { return -1; }
bool power_hal_is_charging(void) { return false; }
bool power_hal_is_vbus_in(void)  { return true; }
bool power_hal_pwr_pressed(void) { return false; }
bool power_hal_pwr_long_pressed(void) { return false; }
bool power_hal_pwr_released(void) { return false; }
