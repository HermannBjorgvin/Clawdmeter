#include "hal/board_info_hal.h"

// Weak default: boards with one hardware revision don't need to implement it.
__attribute__((weak)) const char* board_hal_rev(void) { return ""; }
