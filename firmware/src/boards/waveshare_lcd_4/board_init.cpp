#include "board.h"
#include "io_expander.h"
#include <Arduino.h>
#include <Wire.h>

// Shared I2C (GT911 + expander) then expander rails/backlight. The expander
// MUST come up before display_hal_begin() or the ST7701 stays unpowered.
extern "C" void board_init(void) {
    Wire.begin(IIC_SDA, IIC_SCL);
    io_expander_init();
}
