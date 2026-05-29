#include "../../hal/power_hal.h"
#include "board.h"
#include <Arduino.h>

#define PWR_POLL_MS 50

static bool     pwr_pressed_flag = false;
static bool     last_btn_high    = true;   // INPUT_PULLUP → HIGH when released
static uint32_t last_pwr_ms      = 0;

void power_hal_init(void) {
    pinMode(BTN_BACK_GPIO, INPUT_PULLUP);
    last_btn_high = (digitalRead(BTN_BACK_GPIO) == HIGH);
}

void power_hal_tick(void) {
    uint32_t now = millis();
    if (now - last_pwr_ms < PWR_POLL_MS) return;
    last_pwr_ms = now;

    bool high_now = (digitalRead(BTN_BACK_GPIO) == HIGH);
    if (!high_now && last_btn_high) {
        // Falling edge = button pressed (active-LOW, INPUT_PULLUP)
        pwr_pressed_flag = true;
    }
    last_btn_high = high_now;
}

int  power_hal_battery_pct(void) { return -1; }
bool power_hal_is_charging(void) { return false; }
bool power_hal_is_vbus_in(void)  { return false; }

bool power_hal_pwr_pressed(void) {
    if (pwr_pressed_flag) {
        pwr_pressed_flag = false;
        return true;
    }
    return false;
}
