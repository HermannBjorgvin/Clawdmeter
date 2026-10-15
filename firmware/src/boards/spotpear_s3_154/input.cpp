#include "../../hal/input_hal.h"
#include "board.h"
#include <Arduino.h>

void input_hal_init(void) {
    pinMode(BTN_BACK_GPIO, INPUT_PULLUP);
#if BOARD_HAS_SECONDARY_BUTTON
    // pinMode(BTN_FWD_GPIO, INPUT_PULLUP);   // TODO
#endif
}

bool input_hal_is_held(InputButton btn) {
    switch (btn) {
    case INPUT_BTN_PRIMARY:
        return digitalRead(BTN_BACK_GPIO) == LOW;
    case INPUT_BTN_SECONDARY:
#if BOARD_HAS_SECONDARY_BUTTON
        // return digitalRead(BTN_FWD_GPIO) == LOW;   // TODO
        return false;
#else
        return false;   // not present on this board
#endif
    }
    return false;
}
