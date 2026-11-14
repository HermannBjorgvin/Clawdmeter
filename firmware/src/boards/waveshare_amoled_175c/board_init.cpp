#include "board.h"
#include <Arduino.h>
#include <Wire.h>

// The display and touch resets are separate direct GPIOs.
extern "C" void board_init(void) {
    Wire.begin(IIC_SDA, IIC_SCL);
}
