#include "board.h"
#include <Arduino.h>
#include <Wire.h>
#include <driver/gpio.h>

// Assert the battery power-hold latch first (the board browns out on battery
// otherwise), then bring up the shared I2C bus. Same shape as the LCD-1.54.
extern "C" void board_init(void) {
    // Release the pad hold a power-off left behind (power.cpp latches BAT_EN
    // low before deep sleep), or the HIGH write below never reaches the pin.
    gpio_hold_dis((gpio_num_t)BAT_EN);
    pinMode(BAT_EN, OUTPUT);
    digitalWrite(BAT_EN, HIGH);

    Wire.begin(IIC_SDA, IIC_SCL);
}
