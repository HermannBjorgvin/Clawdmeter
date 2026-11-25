#pragma once
#include <stdint.h>

// Core2 power chip: AXP192 on v1.0, AXP2101 on v1.1, both at I2C 0x34. Raw
// register access (no XPowersLib) so both revisions share one small driver.
// Rails and values follow M5Stack's own Core2 bring-up (M5Unified / M5GFX).

enum PmuKind { PMU_NONE = 0, PMU_AXP192, PMU_AXP2101 };

// Detect the chip, power the panel rails and pulse the panel reset. Wire must be up.
PmuKind pmu_init(void);
PmuKind pmu_kind(void);
const char* pmu_name(void);

// 0 = backlight off, 1..255 = dim..full (the backlight rail's voltage).
void pmu_set_backlight(uint8_t level);

// Speaker amplifier enable (NS4168 SD_MODE line).
void pmu_set_speaker(bool on);

int  pmu_battery_pct(void);   // 0..100, -1 if no battery or unreadable
bool pmu_is_charging(void);
bool pmu_is_vbus_in(void);
