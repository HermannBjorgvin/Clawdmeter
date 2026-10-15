#include "board.h"
#include <Arduino.h>

// No I2C devices are used and no IO expander gates the panel; the backlight pin
// is configured in display_hal_begin(). The power-hold pin is latched HIGH here
// so the board doesn't shut itself off when USB is removed.
extern "C" void board_init(void) {
    pinMode(PWR_LATCH_GPIO, OUTPUT);
    digitalWrite(PWR_LATCH_GPIO, HIGH);
}
