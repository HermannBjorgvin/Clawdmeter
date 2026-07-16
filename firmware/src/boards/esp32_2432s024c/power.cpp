#include "../../hal/power_hal.h"
#include "board.h"
#include "power_button.h"

#include <Arduino.h>

static PowerButtonState button_state{};
static bool pressed_flag = false;
static bool long_pressed_flag = false;
static bool released_flag = false;

void power_hal_init(void) {
    pinMode(BTN_PWR_GPIO, INPUT_PULLUP);
    update_power_button(
        button_state,
        digitalRead(BTN_PWR_GPIO) == LOW,
        millis()
    );
}

void power_hal_tick(void) {
    const PowerButtonEvents events = update_power_button(
        button_state,
        digitalRead(BTN_PWR_GPIO) == LOW,
        millis()
    );
    pressed_flag = pressed_flag || events.pressed;
    long_pressed_flag = long_pressed_flag || events.long_pressed;
    released_flag = released_flag || events.released;
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
