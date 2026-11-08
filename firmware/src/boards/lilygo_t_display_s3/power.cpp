#include "../../hal/power_hal.h"
#include "board.h"
#include <Arduino.h>

// No PMU: battery percentage is estimated from the VBAT divider on
// BAT_ADC_PIN (LiPo voltage curve below). Charging and VBUS state are not
// observable on this board, so both report false.
//
// The PWR-role button is the plain active-LOW GPIO 14. Edges are synthesized
// in software, like the LCD-1.54 port:
//   short    -- fired on release if the hold was shorter than PWR_LONG_MS
//   long     -- fired once when a hold crosses PWR_LONG_MS
//   release  -- fired on every release edge
// so the hold-to-pair gesture in main.cpp stays board-agnostic.

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

static int voltage_to_percent(float vbat) {
    if (vbat < 2.5f) return -1;   // divider floating: no battery connected
    static const struct { float v; int pct; } curve[] = {
        {3.20f, 0}, {3.50f, 5}, {3.60f, 10}, {3.70f, 20}, {3.75f, 35},
        {3.80f, 50}, {3.90f, 65}, {4.00f, 80}, {4.10f, 90}, {4.20f, 100},
    };
    const size_t last = sizeof(curve) / sizeof(curve[0]) - 1;
    if (vbat <= curve[0].v) return 0;
    if (vbat >= curve[last].v) return 100;
    for (size_t i = 1; i <= last; ++i) {
        if (vbat <= curve[i].v) {
            float pos = (vbat - curve[i - 1].v) / (curve[i].v - curve[i - 1].v);
            return curve[i - 1].pct + (int)((curve[i].pct - curve[i - 1].pct) * pos + 0.5f);
        }
    }
    return -1;
}

static void sample_battery(void) {
    // Average a few reads: the divider is high-impedance and single S3 ADC
    // samples are noisy.
    uint32_t mv = 0;
    for (int i = 0; i < 4; i++) mv += analogReadMilliVolts(BAT_ADC_PIN);
    cached_pct = voltage_to_percent((mv / 4) * BAT_VOLT_DIVIDER / 1000.0f);
}

void power_hal_init(void) {
    pinMode(BTN_PWR_GPIO, INPUT_PULLUP);
    analogReadResolution(12);
    analogSetPinAttenuation(BAT_ADC_PIN, ADC_11db);
    sample_battery();
    Serial.printf("Battery ADC init (GPIO%d) pct=%d\n", BAT_ADC_PIN, cached_pct);
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
        if (pwr_now && !last_pwr_state) {            // press edge: hold begins
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
