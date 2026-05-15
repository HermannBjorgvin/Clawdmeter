#include "power.h"
#include "display_cfg.h"
#include <Arduino.h>

// Two paths share this file:
//   BOARD_HAS_PMU         — AXP2101 over I2C (Waveshare): battery % and
//                            charging status come straight from the PMU,
//                            and the PMU's PKEY drives the middle button.
//   BOARD_HAS_BATTERY_ADC — Plain ADC voltage sense (LILYGO): battery
//                            percentage stays unknown (-1) until the
//                            divider ratio is calibrated; we still log
//                            the raw ADC value at boot so calibration is
//                            a one-shot exercise.
//
// Both paths expose the same power_*() API so ui.cpp doesn't care which
// board it's running on.

#if BOARD_HAS_PMU

#define BATTERY_POLL_MS   2000
#define CHARGING_POLL_MS  500
#define PWR_POLL_MS       50

static int      cached_pct      = -1;
static bool     cached_charging = false;
static bool     pwr_pressed_flag = false;
static uint32_t last_battery_ms  = 0;
static uint32_t last_charging_ms = 0;
static uint32_t last_pwr_ms      = 0;

void power_init(void) {
    if (!pmu.begin(Wire, AXP2101_ADDR, IIC_SDA, IIC_SCL)) {
        Serial.println("AXP2101 init failed");
        return;
    }
    Serial.println("AXP2101 init OK");

    pmu.enableBattDetection();
    pmu.enableBattVoltageMeasure();

    pmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
    pmu.clearIrqStatus();
    pmu.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ);

    cached_charging = pmu.isCharging();
    cached_pct = pmu.getBatteryPercent();
}

void power_tick(void) {
    uint32_t now = millis();

    if (now - last_charging_ms >= CHARGING_POLL_MS) {
        last_charging_ms = now;
        cached_charging = pmu.isCharging();
    }

    if (now - last_battery_ms >= BATTERY_POLL_MS) {
        last_battery_ms = now;
        cached_pct = pmu.getBatteryPercent();
    }

    if (now - last_pwr_ms >= PWR_POLL_MS) {
        last_pwr_ms = now;
        pmu.getIrqStatus();
        if (pmu.isPekeyShortPressIrq()) pwr_pressed_flag = true;
        pmu.clearIrqStatus();
    }
}

int  power_battery_pct(void)   { return cached_pct; }
bool power_is_charging(void)   { return cached_charging; }
bool power_pwr_pressed(void) {
    if (pwr_pressed_flag) { pwr_pressed_flag = false; return true; }
    return false;
}

#elif BOARD_HAS_BATTERY_ADC

#define BAT_POLL_MS       2000
static uint32_t last_bat_ms = 0;
static int      cached_raw  = 0;
static float    cached_volt = 0.0f;

static void read_battery(void) {
    cached_raw = analogRead(BAT_ADC_GPIO);
    // V_pin = raw / max * V_ref; V_bat = V_pin * divider * correction
    float v_pin = ((float)cached_raw / (float)BAT_ADC_MAX) * BAT_ADC_REF_VOLTAGE;
    cached_volt = v_pin * BAT_ADC_DIVIDER_RATIO * BAT_ADC_CORRECTION_FACTOR;
}

void power_init(void) {
    analogReadResolution(12);
    pinMode(BAT_ADC_GPIO, INPUT);
    read_battery();
    Serial.printf("BAT ADC init OK (GPIO%d raw=%d v=%.2fV)\n",
                  BAT_ADC_GPIO, cached_raw, cached_volt);
    Serial.println("Battery percent: unknown (ADC un-calibrated)");
}

void power_tick(void) {
    uint32_t now = millis();
    if (now - last_bat_ms < BAT_POLL_MS) return;
    last_bat_ms = now;
    read_battery();
}

// Returning -1 makes ui_update_battery() show the "no battery / unknown"
// icon. Once a real divider ratio + voltage-to-percent curve is in hand,
// drop the conversion in here. Keeping it conservative avoids showing
// wildly wrong percentages on the dashboard.
int  power_battery_pct(void) { return -1; }
bool power_is_charging(void) { return false; }
bool power_pwr_pressed(void) { return false; }

#else

// No battery hardware at all — keep the API alive so callers compile.
void power_init(void)        { Serial.println("power: no battery hardware on this board"); }
void power_tick(void)        {}
int  power_battery_pct(void) { return -1; }
bool power_is_charging(void) { return false; }
bool power_pwr_pressed(void) { return false; }

#endif
