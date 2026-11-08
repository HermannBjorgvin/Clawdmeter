#include "../../hal/power_hal.h"

// No PMU and no PWR-role button (RST is EN, a hardware reset). Battery state
// is unknown (BOARD_HAS_BATTERY=0 hides the indicator). With
// BoardCaps.has_pwr_button=false the pairing hint says to pair from the
// computer and a 5 s touch hold on it clears the bonds (pair_hold.h).

void power_hal_init(void) {}
void power_hal_tick(void) {}

int  power_hal_battery_pct(void)      { return -1; }
bool power_hal_is_charging(void)      { return false; }
bool power_hal_is_vbus_in(void)       { return false; }
bool power_hal_pwr_pressed(void)      { return false; }
bool power_hal_pwr_long_pressed(void) { return false; }
bool power_hal_pwr_released(void)     { return false; }
