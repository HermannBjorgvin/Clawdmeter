#include "../../hal/power_hal.h"
#include "sim_platform.h"

void power_hal_init(void) {}
void power_hal_tick(void) {}

int  power_hal_battery_pct(void) { return sim_battery_pct(); }
bool power_hal_is_charging(void) { return sim_charging(); }
bool power_hal_is_vbus_in(void)  { return sim_charging(); }

bool power_hal_pwr_pressed(void)      { return sim_take_pwr_pressed(); }
bool power_hal_pwr_long_pressed(void) { return sim_take_pwr_long(); }
bool power_hal_pwr_released(void)     { return sim_take_pwr_released(); }
