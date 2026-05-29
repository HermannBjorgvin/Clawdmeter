// Battery reading via GPIO 4 (BAT_ADC).
//
// Divider on this board (from the schematic): VBAT ── R3(200K) ── BAT_ADC ── R15(100K) ── GND
// Ratio 1/3, so V_bat = ADC_voltage * 3.0.
//
// ESP32-S3 ADC1 with 12 dB attenuation reads up to ~2.45 V cleanly. At full
// charge (4.2 V) we expect ~1.4 V at the pin, well inside range.

#include "battery.h"
#include <Arduino.h>

#define BAT_ADC_PIN     4
#define V_FULL_MV       4200   // 4.2 V cell at full charge
#define V_EMPTY_MV      3000   // 3.0 V is our "shutdown" threshold
#define V_CHARGE_THR_MV 4050   // above this → assume USB/charger present

// Light EMA so the icon doesn't twitch with each ADC sample.
static int  s_smoothed_mv = -1;

void battery_init(void) {
    analogReadResolution(12);              // 0..4095
    analogSetPinAttenuation(BAT_ADC_PIN, ADC_11db);  // ~0..2.45 V usable
    // Warm-up sample, discarded.
    (void)analogReadMilliVolts(BAT_ADC_PIN);
}

int battery_sample(int *pct_out, int *charging_out) {
    if (!pct_out || !charging_out) return 0;

    // Average a few reads to smooth ADC noise.
    long acc = 0; const int N = 8;
    for (int i = 0; i < N; i++) acc += analogReadMilliVolts(BAT_ADC_PIN);
    int pin_mv = (int)(acc / N);
    int bat_mv = pin_mv * 3;   // undo divider

    if (s_smoothed_mv < 0) s_smoothed_mv = bat_mv;
    else                   s_smoothed_mv = (s_smoothed_mv * 7 + bat_mv) / 8;

    int pct = ((s_smoothed_mv - V_EMPTY_MV) * 100) / (V_FULL_MV - V_EMPTY_MV);
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;

    *pct_out      = pct;
    *charging_out = (s_smoothed_mv >= V_CHARGE_THR_MV) ? 1 : 0;
    return 1;
}
