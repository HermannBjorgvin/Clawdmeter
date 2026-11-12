#include "../../hal/input_hal.h"

// No application buttons: KEY1 is the hardware reset and KEY2 is BOOT/GPIO 0,
// which doubles as the LCD reset line (see board.h). Touch is the only input.

void input_hal_init(void) {}

bool input_hal_is_held(InputButton btn) {
    (void)btn;
    return false;
}
