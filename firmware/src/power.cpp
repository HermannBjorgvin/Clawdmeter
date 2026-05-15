#include "power.h"
#include <Arduino.h>

// Stub implementation — AXP2101 not available on 1.8" board
// Return safe defaults: no battery, not charging, no button press

void power_init(void) {
    Serial.println("power_init: skipped (AXP2101 not available)");
}

void power_tick(void) {
    // no-op
}

int power_battery_pct(void) {
    return -1;  // no battery info available
}

bool power_is_charging(void) {
    return false;
}

bool power_pwr_pressed(void) {
    return false;  // no PWR button available
}
