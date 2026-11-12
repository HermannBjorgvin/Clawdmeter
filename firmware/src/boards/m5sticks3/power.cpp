#include "../../hal/power_hal.h"
#include "../../hal/display_hal.h"
#include "board.h"
#include "pm1.h"
#include <Arduino.h>

// The M5PM1 PMIC owns the battery (VBAT/VIN telemetry over I2C) and hard
// power (its PEK button: click = on, double-click = off, entirely in
// hardware). The PWR *role* — screen toggle, hold-to-pair — lives on the
// side button (BtnB) with the same software edge synthesis as the LCD-1.54
// port. Holding BtnB to 8 s asks the PM1 for a full shutdown, mirroring the
// AXP boards' 8-second hardware power-off.

#define BATTERY_POLL_MS  2000
#define PWR_POLL_MS      50
#define PWR_LONG_MS      1500
#define PWR_OFF_HOLD_MS  8000

static int      cached_pct        = -1;
static bool     cached_vbus       = false;
static bool     pwr_pressed_flag  = false;
static bool     pwr_long_flag     = false;
static bool     pwr_released_flag = false;
static bool     last_pwr_state    = false;
static uint32_t pwr_press_started_ms = 0;
static bool     pwr_long_fired    = false;
static uint32_t last_battery_ms   = 0;
static uint32_t last_pwr_ms       = 0;

static void sample_battery(void) {
    int mv = pm1_read16(PM1_REG_VBAT_L);
    if (mv < 3000) {   // I2C error (-1) or no battery — divider floats low
        cached_pct = -1;
    } else {
        // Linear 3.3 V → 0%, 4.2 V → 100%. Crude but serviceable for a
        // four-state indicator icon on a 250 mAh cell.
        int pct = (int)((mv / 1000.0f - 3.3f) * (100.0f / 0.9f) + 0.5f);
        cached_pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    }
    int vin = pm1_read16(PM1_REG_VIN_L);
    cached_vbus = vin > 4000;   // USB present when VIN sits near 5 V
}

static void power_off(void) {
    Serial.println("PWR held 8s — asking the PM1 to shut down");
    Serial.flush();
    display_hal_set_brightness(0);
    delay(50);
    pm1_write8(PM1_REG_SYS_CMD, PM1_SYS_CMD_SHUTDOWN);
    delay(1000);   // the PMIC cuts the rail; nothing to do if it doesn't
}

void power_hal_init(void) {
    pinMode(BTN_B_GPIO, INPUT_PULLUP);
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
        bool pwr_now = (digitalRead(BTN_B_GPIO) == LOW);   // active LOW
        if (pwr_now && !last_pwr_state) {            // press edge — hold begins
            pwr_press_started_ms = now;
            pwr_long_fired = false;
        } else if (pwr_now && last_pwr_state) {      // held
            if (!pwr_long_fired && (now - pwr_press_started_ms >= PWR_LONG_MS)) {
                pwr_long_flag  = true;
                pwr_long_fired = true;
            }
            if (now - pwr_press_started_ms >= PWR_OFF_HOLD_MS) {
                power_off();
            }
        } else if (!pwr_now && last_pwr_state) {     // release edge
            pwr_released_flag = true;
            if (!pwr_long_fired) pwr_pressed_flag = true;  // short press
        }
        last_pwr_state = pwr_now;
    }
}

int  power_hal_battery_pct(void) { return cached_pct; }
bool power_hal_is_vbus_in(void)  { return cached_vbus; }
bool power_hal_is_charging(void) {
    // The PM1 charge-state register isn't documented; USB present with a
    // less-than-full battery is the honest approximation for the icon.
    return cached_vbus && cached_pct >= 0 && cached_pct < 100;
}

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
