#pragma once

void power_init(void);
void power_tick(void);
int  power_battery_pct(void);       // 0-100, or -1 if no battery
bool power_is_charging(void);
bool power_pwr_pressed(void);       // short press
bool power_pwr_long_pressed(void);  // long press (>1.5s typically)
void power_shutdown(void);          // tell AXP2101 to cut power
