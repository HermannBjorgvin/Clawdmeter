#include "board.h"

// Nothing to bring up before the display: no I2C devices are used, no IO
// expander gates the panel, and the backlight pin is configured in
// display_hal_begin().
extern "C" void board_init(void) {}
