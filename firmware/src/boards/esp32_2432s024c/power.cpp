#include "../../hal/power_hal.h"
#include "board.h"

#include <Arduino.h>

static bool button_down = false;
static bool long_press_sent = false;
static uint32_t button_down_since_ms = 0;
static bool pressed_flag = false;
static bool long_pressed_flag = false;
static bool released_flag = false;

void power_hal_init(void) {
    pinMode(BTN_PWR_GPIO, INPUT_PULLUP);
    button_down = digitalRead(BTN_PWR_GPIO) == LOW;
    button_down_since_ms = millis();
}

void power_hal_tick(void) {
    const bool down = digitalRead(BTN_PWR_GPIO) == LOW;
    const uint32_t now_ms = millis();

    if (down && !button_down) {
        button_down = true;
        long_press_sent = false;
        button_down_since_ms = now_ms;
    } else if (down && !long_press_sent &&
               now_ms - button_down_since_ms >= 1500) {
        long_press_sent = true;
        long_pressed_flag = true;
    } else if (!down && button_down) {
        pressed_flag = pressed_flag || !long_press_sent;
        released_flag = true;
        button_down = false;
    }
}

int power_hal_battery_pct(void) { return -1; }
bool power_hal_is_charging(void) { return false; }
bool power_hal_is_vbus_in(void) { return true; }

bool power_hal_pwr_pressed(void) {
    const bool value = pressed_flag;
    pressed_flag = false;
    return value;
}

bool power_hal_pwr_long_pressed(void) {
    const bool value = long_pressed_flag;
    long_pressed_flag = false;
    return value;
}

bool power_hal_pwr_released(void) {
    const bool value = released_flag;
    released_flag = false;
    return value;
}
