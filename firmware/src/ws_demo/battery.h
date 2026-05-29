// Battery telemetry for the 3.49 board. No PMU IC on this hardware, so we
// read a voltage divider on GPIO 4 (NLBAT0ADC / IO4 per the schematic) and
// estimate state-of-charge linearly between 3.0 V empty and 4.2 V full.
//
// Charging detection: ETA6098 charger STAT pin isn't exposed on a GPIO this
// board surface easily, so we infer charging from the measured voltage. If
// V_bat is above the typical resting full mark (~4.05 V) we assume external
// power is in. This will report "charging" while plugged in even on a
// well-rested full battery, which is acceptable for an indicator icon.

#pragma once
#ifdef __cplusplus
extern "C" {
#endif

void battery_init(void);

// Take a fresh reading. Returns SoC percent 0..100 in *pct (clamped) and
// charging state in *charging. Returns 1 on success, 0 on error.
int  battery_sample(int *pct, int *charging);

#ifdef __cplusplus
}
#endif
