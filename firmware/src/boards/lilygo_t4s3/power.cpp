#include "../../hal/power_hal.h"
#include "board.h"
#include <Arduino.h>
#include <Wire.h>
#include <XPowersLib.h>

// The SY6970 is a charger IC, not a fuel gauge — no native percent
// reading is available. We approximate from the battery voltage, which is
// rough but acceptable for a desk indicator. Range is calibrated for a
// single-cell LiPo: 4.20 V → 100 %, 3.30 V → 0 %.
//
// PWR-button synthesis: the board has no PMU push-button and no second
// GPIO button, so screen cycling is mapped to a long press of GPIO 0
// (the same button input.cpp reports as PRIMARY). The two HALs observe
// the same GPIO independently — a brief press is just a Space PTT; a
// hold past PWR_LONGPRESS_MS fires one PWR press event (edge on release)
// without disturbing the in-flight HID press/release semantics.

#define BATTERY_POLL_MS  2000
#define CHARGING_POLL_MS 500
#define PWR_POLL_MS      20      // tight enough to catch a deliberate long press

static PowersSY6970 pmu;
static bool pmu_ok = false;

static int      cached_pct       = -1;
static bool     cached_charging  = false;
static bool     cached_vbus      = false;
static bool     pwr_pressed_flag = false;
static uint32_t last_battery_ms  = 0;
static uint32_t last_charging_ms = 0;
static uint32_t last_pwr_ms      = 0;

// Long-press tracker for GPIO 0.
static bool     btn_was_down       = false;
static uint32_t btn_press_start_ms = 0;
static bool     btn_long_consumed  = false;

static int approx_pct_from_mv(uint16_t mv) {
    if (mv == 0) return -1;        // ADC not yet scanned / no battery
    if (mv >= 4200) return 100;
    if (mv <= 3300) return 0;
    return (int)((mv - 3300) * 100UL / (4200 - 3300));
}

void power_hal_init(void) {
    // PMICEN is already HIGH from board_init(); the SY6970 talks I2C
    // straight away. PowersSY6970 uses init() rather than begin().
    if (!pmu.init(Wire, IIC_SDA, IIC_SCL, SY6970_ADDR)) {
        Serial.println("SY6970 init failed");
        return;
    }
    pmu.enableADCMeasure();   // otherwise getBattVoltage() returns 0
    pmu_ok = true;
    Serial.println("SY6970 init OK");

    cached_charging = pmu.isCharging();
    cached_vbus     = pmu.isVbusIn();
    cached_pct      = approx_pct_from_mv(pmu.getBattVoltage());
}

void power_hal_tick(void) {
    uint32_t now = millis();

    if (pmu_ok) {
        if (now - last_charging_ms >= CHARGING_POLL_MS) {
            last_charging_ms = now;
            cached_charging = pmu.isCharging();
            cached_vbus     = pmu.isVbusIn();
        }
        if (now - last_battery_ms >= BATTERY_POLL_MS) {
            last_battery_ms = now;
            cached_pct = approx_pct_from_mv(pmu.getBattVoltage());
        }
    }

    if (now - last_pwr_ms >= PWR_POLL_MS) {
        last_pwr_ms = now;
        bool down = (digitalRead(BTN_BACK_GPIO) == LOW);
        if (down && !btn_was_down) {
            btn_press_start_ms = now;
            btn_long_consumed = false;
        } else if (down && !btn_long_consumed &&
                   now - btn_press_start_ms >= PWR_LONGPRESS_MS) {
            pwr_pressed_flag = true;
            btn_long_consumed = true;
        }
        btn_was_down = down;
    }
}

int  power_hal_battery_pct(void) { return cached_pct; }
bool power_hal_is_charging(void) { return cached_charging; }
bool power_hal_is_vbus_in(void)  { return cached_vbus; }

bool power_hal_pwr_pressed(void) {
    if (pwr_pressed_flag) {
        pwr_pressed_flag = false;
        return true;
    }
    return false;
}
