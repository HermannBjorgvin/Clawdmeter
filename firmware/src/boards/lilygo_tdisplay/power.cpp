#include "../../hal/power_hal.h"
#include "board.h"
#include <Arduino.h>

// No PMU: the charger is standalone and exposes nothing. Battery percentage
// comes from the VBAT divider on BAT_ADC_PIN; charging / VBUS state is
// unknowable, so both report false.
//
// The PWR-role button is a plain active-LOW GPIO turned into the three edges
// main.cpp's board-agnostic gesture logic expects (same software synthesis as
// the LCD-1.54 and DevKit ports):
//   short   — on release, if the hold was shorter than PWR_LONG_MS
//   long    — once, when a hold crosses PWR_LONG_MS
//   release — on every release edge
// short = toggle splash <-> usage (this board has no touch, so the button owns
// that gesture); long-hold ~3 s then release = clear the BLE bond and
// re-advertise.
//
// No power-off: unlike the LCD-1.54 there is no BAT_EN power-hold latch to
// drop, so there is no rail this board could cut for itself.

#define BATTERY_POLL_MS  2000
#define PWR_POLL_MS      50
#define PWR_LONG_MS      1500

static int      cached_pct        = -1;
static bool     pwr_pressed_flag  = false;
static bool     pwr_long_flag     = false;
static bool     pwr_released_flag = false;
static bool     last_pwr_state    = false;
static bool     pwr_long_fired    = false;
static uint32_t pwr_press_started_ms = 0;
static uint32_t last_battery_ms   = 0;
static uint32_t last_pwr_ms       = 0;

static void sample_battery(void) {
    // Average a few reads — the divider is high-impedance and single ADC
    // samples are noisy.
    uint32_t mv = 0;
    for (int i = 0; i < 4; i++) mv += analogReadMilliVolts(BAT_ADC_PIN);
    float vbat = (mv / 4) * BAT_VOLT_DIVIDER / 1000.0f;

    if (vbat < 3.0f) {          // divider pulled down — nothing on the rail
        cached_pct = -1;
        return;
    }
    // Linear 3.3 V → 0%, 4.2 V → 100%. Crude but serviceable for a four-state
    // indicator icon. On USB this pins at 100% whether or not a cell is
    // attached (see board.h) — the divider sits on the charger output.
    int pct = (int)((vbat - 3.3f) * (100.0f / 0.9f) + 0.5f);
    cached_pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
}

void power_hal_init(void) {
    // GPIO35 is input-only: no internal pull-up exists, and the board carries
    // an external one (see board.h). INPUT_PULLUP here would be a silent no-op
    // on this pin, so be explicit about relying on the board's resistor.
#if BTN_PWR_INPUT_ONLY
    pinMode(BTN_PWR_GPIO, INPUT);
#else
    pinMode(BTN_PWR_GPIO, INPUT_PULLUP);
#endif
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
        if (pwr_now && !last_pwr_state) {                    // press edge
            pwr_press_started_ms = now;
            pwr_long_fired = false;
        } else if (pwr_now && last_pwr_state) {              // held
            if (!pwr_long_fired && (now - pwr_press_started_ms >= PWR_LONG_MS)) {
                pwr_long_flag  = true;
                pwr_long_fired = true;
            }
        } else if (!pwr_now && last_pwr_state) {             // release edge
            pwr_released_flag = true;
            if (!pwr_long_fired) pwr_pressed_flag = true;    // short press
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
