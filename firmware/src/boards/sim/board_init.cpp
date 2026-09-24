#include "board.h"

// No I2C bus, no IO expander — nothing to bring up before the HAL inits.
extern "C" void board_init(void) {}
