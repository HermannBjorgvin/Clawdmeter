#include "../../hal/input_hal.h"
#include "board.h"
#include <Arduino.h>

// Only the BOOT button (GPIO0) is a screen-independent input. PWR comes through
// power_hal (AXP2101 PKEY). No secondary button on this board.

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
