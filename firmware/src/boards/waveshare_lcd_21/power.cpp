#include "../../hal/power_hal.h"
#include "board.h"
#include <Arduino.h>

// No PMU: the ETA6098 charger is standalone and exposes nothing over I2C.
// Battery percentage comes from the 200k/100k divider on BAT_ADC_PIN;
// charging state is unknowable. SW1 is a battery slide switch and RESET is
// EN, so there is no PWR-role button and the hold-to-pair gesture is the
// 5 s touch hold on the pairing hint (has_pwr_button = false).
//
// VBUS isn't wired to a GPIO either. With no battery read the board can only
// be running from USB, so that case reports VBUS present and idle sleep stays
// off on a battery-less desk unit (IDLE_SLEEP_WHEN_CHARGING is false).

#define BATTERY_POLL_MS  2000

static int      cached_pct      = -1;
static uint32_t last_battery_ms = 0;

static void sample_battery(void) {
    // Average a few reads — the divider is high-impedance and single ADC
    // samples on the S3 are noisy.
    uint32_t mv = 0;
    for (int i = 0; i < 4; i++) mv += analogReadMilliVolts(BAT_ADC_PIN);
    float vbat = (mv / 4) * BAT_VOLT_DIVIDER / 1000.0f;

    if (vbat < 3.0f) {          // divider floating — no battery connected
        cached_pct = -1;
        return;
    }
    // Linear 3.3 V → 0%, 4.2 V → 100%. Crude but serviceable for a
    // four-state indicator icon.
    int pct = (int)((vbat - 3.3f) * (100.0f / 0.9f) + 0.5f);
    cached_pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
}

void power_hal_init(void) {
    analogReadResolution(12);
    sample_battery();
}

void power_hal_tick(void) {
    uint32_t now = millis();
    if (now - last_battery_ms >= BATTERY_POLL_MS) {
        last_battery_ms = now;
        sample_battery();
    }
}

int  power_hal_battery_pct(void) { return cached_pct; }
bool power_hal_is_charging(void) { return false; }
bool power_hal_is_vbus_in(void)  { return cached_pct < 0; }
bool power_hal_pwr_pressed(void) { return false; }
bool power_hal_pwr_long_pressed(void) { return false; }
bool power_hal_pwr_released(void) { return false; }
