#include "../../hal/power_hal.h"
#include "board.h"
#include <Arduino.h>

// The T4-S3 uses a SY6970 charger IC (not AXP2101). Battery percentage is
// not readable without external calibration, so we return -1 and the UI hides
// the battery indicator. Charging state is available via the LilyGo API.
//
// There is no AXP2101 PKEY — the only physical button is the BOOT button
// (GPIO 0), which is handled as INPUT_BTN_PRIMARY by input_hal. All
// power_hal_pwr_* functions return false.

#define CHARGING_POLL_MS 1000

static bool     cached_charging = false;
static uint32_t last_charging_ms = 0;

void power_hal_init(void) {
    cached_charging = amoled.isCharging();
    Serial.println("Power HAL init OK (SY6970)");
}

void power_hal_tick(void) {
    uint32_t now = millis();
    if (now - last_charging_ms >= CHARGING_POLL_MS) {
        last_charging_ms = now;
        cached_charging  = amoled.isCharging();
    }
}

int  power_hal_battery_pct(void)      { return -1; }
bool power_hal_is_charging(void)      { return cached_charging; }
bool power_hal_pwr_pressed(void)      { return false; }
bool power_hal_pwr_long_pressed(void) { return false; }
bool power_hal_pwr_released(void)     { return false; }
