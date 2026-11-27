#include "board.h"
#include "io_expander.h"
#include <Arduino.h>
#include <Wire.h>

// The ST7796 reset is on the TCA9554 (EXIO1), so the expander comes up before
// display_hal_init(). The FT6336 has no reset line.

extern "C" void board_init(void) {
    Wire.begin(IIC_SDA, IIC_SCL);
    io_expander_init();
}
