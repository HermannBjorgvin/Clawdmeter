#pragma once

// Pull any PMIC-enable rail HIGH before the display (or other peripherals)
// power up. On the LilyGO T4-S3 the panel only has power once GPIO 9 is
// HIGH, so this must run before gfx->begin(). No-op on boards without a
// PMICEN pin.
void power_early_enable(void);

void power_init(void);
void power_tick(void);
int  power_battery_pct(void);    // 0-100, or -1 if no battery
bool power_is_charging(void);
bool power_pwr_pressed(void);    // true once per AXP2101 PWR button short-press
