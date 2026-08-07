#include "board.h"
#include <Arduino.h>
#include <Wire.h>

// Nothing gates the panel on this kit: no IO expander, no power-hold latch,
// and the LCD reset is a direct GPIO that Arduino_ST7789 pulses itself. So
// board_init() only has to bring up the I2C bus the touch controller lives on
// (touch reset is pulsed in touch_hal_init()).
extern "C" void board_init(void) {
    Wire.begin(IIC_SDA, IIC_SCL);
}
