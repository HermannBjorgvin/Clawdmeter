#include "../../hal/input_hal.h"
#include "board.h"
#include <Arduino.h>

// AMOLED-1.8 has only the BOOT button as a secondary input — the PWR
// button comes through power_hal (XCA9554 EXIO4). No secondary button.

void input_hal_init(void) {
    pinMode(BTN_BACK_GPIO, INPUT_PULLUP);
}

bool input_hal_is_held(InputButton btn) {
    switch (btn) {
    case INPUT_BTN_PRIMARY:
        return digitalRead(BTN_BACK_GPIO) == LOW;
    case INPUT_BTN_SECONDARY:
        return false;   // not present on this board
    }
    return false;
}
