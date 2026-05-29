#include "../../hal/power_hal.h"
#include "board.h"
#include <Arduino.h>

void power_hal_init(void) {}
void power_hal_tick(void) {}

int  power_hal_battery_pct(void) { return -1; }
bool power_hal_is_charging(void) { return false; }
bool power_hal_is_vbus_in(void)  { return false; }
bool power_hal_pwr_pressed(void) { return false; }
