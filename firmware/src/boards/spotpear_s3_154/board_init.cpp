#include "board.h"
#include <Arduino.h>
#include <Wire.h>

// Brings up the touch I2C bus. No IO expander on this board; the touch reset
// GPIO is pulsed in touch_hal_init().
extern "C" void board_init(void) {
    Wire.begin(IIC_SDA, IIC_SCL);
}
