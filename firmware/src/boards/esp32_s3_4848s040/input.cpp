#include "../../hal/input_hal.h"

// No button the firmware can read: BOOT and RST are physically inaccessible,
// GPIO 0 is display R4, and the front SW1 is wired to the IP5306's KEY pin.
// Touch is the only input path.

void input_hal_init(void) {}
bool input_hal_is_held(InputButton btn) { (void)btn; return false; }
