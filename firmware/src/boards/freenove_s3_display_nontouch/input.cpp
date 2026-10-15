#include "../../hal/input_hal.h"

// The BOOT button is the PWR-role input (see power.cpp), so there is no
// primary (Space / voice mode) or secondary button.

void input_hal_init(void) {}

bool input_hal_is_held(InputButton btn) {
    (void)btn;
    return false;
}
