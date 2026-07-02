#pragma once
#include <stdint.h>

// Minimal BQ27220 fuel-gauge reader (standard word-register reads, no
// configuration/calibration support). Board-private to this port; not
// exposed in hal/. Replaces the AXP2101 PMU used on the other boards —
// this board has no PMU, battery % and charge state come from this chip.

bool fuel_gauge_init(void);
int  fuel_gauge_read_pct(void);          // 0..100, or -1 on read failure
int  fuel_gauge_read_current_ma(void);   // signed; positive = charging
