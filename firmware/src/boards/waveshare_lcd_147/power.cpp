#include "../../hal/power_hal.h"
#include "board.h"
#include <Arduino.h>

// No PMU on this kit — the charger is a standalone part that exposes nothing
// over I2C. Battery percentage comes from the VBAT divider on BAT_ADC_PIN
// (GPIO12 = ADC2_CH1; ADC2 is only contended by Wi-Fi, which this firmware
// never starts, so BLE-only operation reads it fine). Charging / VBUS state is
// unknowable, so both report false.
//
// The PWR-role button is the BOOT key (GPIO 0), a plain active-LOW GPIO — the
// kit's only readable button, see board.h. Same software edge synthesis as the
// LCD-1.54 port:
//   short    — fired on release if the hold was shorter than PWR_LONG_MS
//   long     — fired once when a hold crosses PWR_LONG_MS
//   release  — fired on every release edge
// which keeps the hold-to-pair gesture in main.cpp board-agnostic.
//
// Unlike the LCD-1.54 there is deliberately NO hold-to-power-off: this board
// has no power-hold latch to drop (so "off" could only ever mean deep sleep),
// and GPIO 0 is the ESP32-S3's boot strapping pin — waking from deep sleep on
// an ext0 LOW level means the ROM samples GPIO 0 while the button is still
// held and drops the chip into USB download mode, which looks like a brick to
// the user. Holding past the pairing window simply does nothing here.

#define BATTERY_POLL_MS  2000
#define PWR_POLL_MS      50
#define PWR_LONG_MS      1500

static int      cached_pct        = -1;
static bool     pwr_pressed_flag  = false;
static bool     pwr_long_flag     = false;
static bool     pwr_released_flag = false;
static bool     last_pwr_state    = false;
static uint32_t pwr_press_started_ms = 0;
static bool     pwr_long_fired    = false;
static uint32_t last_battery_ms   = 0;
static uint32_t last_pwr_ms       = 0;

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
    pinMode(BTN_PWR_GPIO, INPUT_PULLUP);
    analogReadResolution(12);
    sample_battery();
}

void power_hal_tick(void) {
    uint32_t now = millis();

    if (now - last_battery_ms >= BATTERY_POLL_MS) {
        last_battery_ms = now;
        sample_battery();
    }
    if (now - last_pwr_ms >= PWR_POLL_MS) {
        last_pwr_ms = now;
        bool pwr_now = (digitalRead(BTN_PWR_GPIO) == LOW);   // active LOW
        if (pwr_now && !last_pwr_state) {            // press edge — hold begins
            pwr_press_started_ms = now;
            pwr_long_fired = false;
        } else if (pwr_now && last_pwr_state) {      // held
            if (!pwr_long_fired && (now - pwr_press_started_ms >= PWR_LONG_MS)) {
                pwr_long_flag  = true;
                pwr_long_fired = true;
            }
        } else if (!pwr_now && last_pwr_state) {     // release edge
            pwr_released_flag = true;
            if (!pwr_long_fired) pwr_pressed_flag = true;  // short press
        }
        last_pwr_state = pwr_now;
    }
}

int  power_hal_battery_pct(void) { return cached_pct; }
bool power_hal_is_charging(void) { return false; }
bool power_hal_is_vbus_in(void)  { return false; }

bool power_hal_pwr_pressed(void) {
    if (pwr_pressed_flag) { pwr_pressed_flag = false; return true; }
    return false;
}

bool power_hal_pwr_long_pressed(void) {
    if (pwr_long_flag) { pwr_long_flag = false; return true; }
    return false;
}

bool power_hal_pwr_released(void) {
    if (pwr_released_flag) { pwr_released_flag = false; return true; }
    return false;
}
