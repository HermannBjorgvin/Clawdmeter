#include "board.h"
#include <Arduino.h>

// Xingzhi Cube 1.83 has no I2C peripherals on the buses Clawdmeter cares
// about (touch / PMU / IMU are all absent). We still need to latch the
// power-hold pin HIGH here so the board doesn't shut itself off the
// moment USB is removed.
extern "C" void board_init(void) {
    pinMode(PWR_LATCH_GPIO, OUTPUT);
    digitalWrite(PWR_LATCH_GPIO, HIGH);
}
