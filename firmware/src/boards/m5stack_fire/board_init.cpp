#include "board.h"
#include <Arduino.h>
#include <Wire.h>

// Bring up the internal I2C bus (IP5306 PMU + MPU6886 IMU). No IO expander and
// no power-hold latch on this board — the IP5306 keeps the rail up on its own —
// so board_init just starts Wire. The display reset is a direct GPIO pulsed by
// the Arduino_GFX driver in display_hal_begin(), so nothing else is needed here.
extern "C" void board_init(void) {
    Wire.begin(IIC_SDA, IIC_SCL);
}
