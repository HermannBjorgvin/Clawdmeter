#pragma once

void power_init(void);
void power_tick(void);
int  power_battery_pct(void);    // 0-100, or -1 if no battery
bool power_is_charging(void);
bool power_is_usb_connected(void);
bool power_usb_changed(void);    // true once per VBUS state transition
bool power_pwr_pressed(void);    // true once per AXP2101 PWR button short-press
