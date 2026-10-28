#include "../../hal/input_hal.h"

// The single BOOT button is wired to the PWR role (power.cpp), not to HID.
// See the button rationale in board.h; caps.button_count is 0 to match.
void input_hal_init(void) {}

bool input_hal_is_held(InputButton btn) {
    (void)btn;
    return false;
}
