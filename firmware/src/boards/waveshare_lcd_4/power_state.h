#pragma once
// LCD-4 board-private glue between power.cpp and display.cpp.

// True when running from the battery (no USB power detected).
bool lcd4_on_battery(void);

// Re-apply the last requested brightness (e.g. after the power source
// changes, so the on-battery cap takes effect immediately).
void lcd4_reapply_brightness(void);
