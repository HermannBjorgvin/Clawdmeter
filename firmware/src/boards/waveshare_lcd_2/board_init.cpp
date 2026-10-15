#include "board.h"
#include <Arduino.h>
#include <Wire.h>

// Bring up the I2C bus shared by touch and the IMU, and light the backlight
// before the panel comes up so the first frame is not a black flash. The
// panel reset is pulsed by gfx->begin().
extern "C" void board_init(void) {
    pinMode(LCD_BL, OUTPUT);
    digitalWrite(LCD_BL, HIGH);
    Wire.begin(IIC_SDA, IIC_SCL);
}
