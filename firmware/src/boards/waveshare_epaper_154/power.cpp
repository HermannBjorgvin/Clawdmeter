#include "../../hal/power_hal.h"
#include "board.h"
#include <Arduino.h>

// SSD1681 e-paper board: NO PMU / fuel gauge. Battery voltage is sensed on
// VBAT_ADC_GPIO (GPIO4 = ADC1_CH3) behind a 1:2 divider — the firmware
// reconstructs the pack voltage as (measured pin mV * VBAT_ADC_DIVIDER),
// mirroring Waveshare's 01_ADC_Test/adc_bsp.cpp. GPIO4 is ADC1, so it is
// safe to read with the NimBLE radio active.
//
// We have no AXP-style fuel gauge, so percent is derived from a single-cell
// Li-ion open-circuit-voltage curve (mv_to_pct) plus an EMA and per-icon
// hysteresis. The hysteresis matters on e-paper: ui_update_battery maps the
// percent to one of 5 icons, and main.cpp repaints whenever the percent
// changes — without hysteresis, sub-mV jitter near an icon boundary would
// queue a partial refresh every poll and needlessly age the panel.
//
// is_charging stays false: this board breaks out no charge-status pin, and
// we won't fabricate a charging state. vbus_in stays true on purpose — the
// idle state machine gates display_hal_tick on it, and reporting false would
// let the panel auto-sleep and freeze the bistable e-paper (see idle_cfg.h /
// power_hal.h). PWR button is a direct GPIO, edge-detected with debounce.
//
// VBAT_PWR (GPIO17) is the active-HIGH soft-power latch driven in
// board_init.cpp — a power ENABLE, not a sense line. Don't read level from it.

#define PWR_POLL_MS      50
#define BATTERY_POLL_MS  2000
#define ADC_SAMPLES      5       // median-of-N per poll to reject single-sample spikes
#define HYST_PCT         3       // icon-boundary hysteresis, in percent

static bool     pwr_pressed_flag = false;
static bool     last_pwr_state   = false;
static uint32_t last_pwr_ms      = 0;
static uint32_t last_batt_ms     = 0;

static float    ema_mv     = 0.0f;   // smoothed Vbat in millivolts
static bool     ema_init   = false;
static int      cached_pct = -1;     // -1 until the first valid read

// Single-cell Li-ion OCV approximation (mV -> percent), high to low,
// linearly interpolated between points. Coarse but honest — not a coulomb
// counter; under load the IR drop reads a few percent low, fine for a
// 5-state icon.
static const struct { int mv; int pct; } LIPO[] = {
    {4200, 100}, {4100, 90}, {4000, 80}, {3900, 65}, {3800, 50},
    {3700, 33},  {3600, 20}, {3500, 12}, {3400, 6},  {3300, 2}, {3270, 0},
};

static int mv_to_pct(int mv) {
    const int n = sizeof(LIPO) / sizeof(LIPO[0]);
    if (mv >= LIPO[0].mv)     return 100;
    if (mv <= LIPO[n - 1].mv) return 0;
    for (int i = 0; i < n - 1; i++) {
        if (mv <= LIPO[i].mv && mv >= LIPO[i + 1].mv) {
            int dv = LIPO[i].mv  - LIPO[i + 1].mv;
            int dp = LIPO[i].pct - LIPO[i + 1].pct;
            return LIPO[i + 1].pct + (int)((long)(mv - LIPO[i + 1].mv) * dp / dv);
        }
    }
    return 0;
}

// Median of ADC_SAMPLES calibrated reads, scaled back up through the divider.
static int read_vbat_mv(void) {
    uint32_t s[ADC_SAMPLES];
    for (int i = 0; i < ADC_SAMPLES; i++)
        s[i] = analogReadMilliVolts(VBAT_ADC_GPIO);   // factory-calibrated pin mV
    for (int i = 1; i < ADC_SAMPLES; i++) {           // insertion sort
        uint32_t v = s[i];
        int j = i - 1;
        while (j >= 0 && s[j] > v) { s[j + 1] = s[j]; j--; }
        s[j + 1] = v;
    }
    return (int)s[ADC_SAMPLES / 2] * VBAT_ADC_DIVIDER;
}

// Map a percent to its 5-state-icon bucket (matches ui_update_battery's
// thresholds: <=10 empty, <=35 low, <=75 medium, else full).
static int bucket_of(int pct) {
    if (pct <= 10) return 0;
    if (pct <= 35) return 1;
    if (pct <= 75) return 2;
    return 3;
}

// Only let cached_pct change icon bucket once the EMA crosses the boundary
// by more than HYST_PCT, so a reading hovering on a boundary doesn't flap.
static int apply_hysteresis(int raw_pct) {
    if (cached_pct < 0) return raw_pct;          // first read: accept as-is
    int cur_b = bucket_of(cached_pct);
    int new_b = bucket_of(raw_pct);
    if (new_b == cur_b) return cached_pct;       // same bucket: hold steady
    if (new_b > cur_b) {                          // rising into a higher bucket
        int bnd = (cur_b == 0) ? 10 : (cur_b == 1) ? 35 : 75;
        if (raw_pct < bnd + HYST_PCT) return cached_pct;
    } else {                                      // falling into a lower bucket
        int bnd = (cur_b == 1) ? 10 : (cur_b == 2) ? 35 : 75;
        if (raw_pct > bnd - HYST_PCT) return cached_pct;
    }
    return raw_pct;
}

void power_hal_init(void) {
    pinMode(BTN_PWR_GPIO, INPUT_PULLUP);
    last_pwr_state = (digitalRead(BTN_PWR_GPIO) == LOW);

    analogReadResolution(12);
    analogSetPinAttenuation(VBAT_ADC_GPIO, ADC_11db);   // ~3.3V full-scale (12 dB)

    int mv = read_vbat_mv();
    ema_mv = (float)mv;
    ema_init = true;
    cached_pct = mv_to_pct(mv);                          // seed without hysteresis
}

void power_hal_tick(void) {
    uint32_t now = millis();

    if (now - last_pwr_ms >= PWR_POLL_MS) {
        last_pwr_ms = now;
        bool pwr_now = (digitalRead(BTN_PWR_GPIO) == LOW);
        if (pwr_now && !last_pwr_state) pwr_pressed_flag = true;
        last_pwr_state = pwr_now;
    }

    if (now - last_batt_ms >= BATTERY_POLL_MS) {
        last_batt_ms = now;
        int mv = read_vbat_mv();
        if (!ema_init) { ema_mv = (float)mv; ema_init = true; }
        else           { ema_mv = ema_mv * 0.8f + (float)mv * 0.2f; }
        int raw_pct = mv_to_pct((int)(ema_mv + 0.5f));
        if (raw_pct < 0)   raw_pct = 0;
        if (raw_pct > 100) raw_pct = 100;
        cached_pct = apply_hysteresis(raw_pct);
    }
}

int  power_hal_battery_pct(void) { return cached_pct; }
bool power_hal_is_charging(void) { return false; }   // no charge-status pin broken out
bool power_hal_is_vbus_in(void)  { return true; }     // keep idle SM awake (see top-of-file)

bool power_hal_pwr_pressed(void) {
    if (pwr_pressed_flag) { pwr_pressed_flag = false; return true; }
    return false;
}

bool power_hal_pwr_long_pressed(void) { return false; }
bool power_hal_pwr_released(void)     { return false; }
