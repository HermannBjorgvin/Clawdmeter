#include "../../hal/power_hal.h"

// No PMU, no battery gauge and no PWR button reach the firmware on this board:
// the only user key is BOOT (Space / push-to-talk, read by input_hal) and the
// other key is a hardware reset. BoardCaps.has_pwr_button is therefore false,
// so shared code explains pairing from the computer and offers the 5 s touch
// hold on the pairing hint to clear bonds (pair_hold.h), the same arrangement
// as the LCD-4. Every power HAL call reports "not available".

void power_hal_init(void) {}
void power_hal_tick(void) {}

int  power_hal_battery_pct(void) { return -1; }
bool power_hal_is_charging(void) { return false; }
bool power_hal_is_vbus_in(void)  { return false; }

bool power_hal_pwr_pressed(void)      { return false; }
bool power_hal_pwr_long_pressed(void) { return false; }
bool power_hal_pwr_released(void)     { return false; }
