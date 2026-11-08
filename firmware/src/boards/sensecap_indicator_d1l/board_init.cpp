#include "board.h"
#include "io_expander.h"
#include <Arduino.h>
#include <Wire.h>

extern "C" void board_init(void) {
    Wire.begin(IIC_SDA, IIC_SCL);
    io_expander_init();
}
