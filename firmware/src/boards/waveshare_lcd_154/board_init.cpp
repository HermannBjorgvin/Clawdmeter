#include "board.h"
#include <Arduino.h>
#include <Wire.h>
#include <driver/gpio.h>

// Bring up the shared I2C bus and assert the battery power-hold line. BAT_EN
// must go HIGH before anything power-hungry runs or the board browns out when
// running from battery (it is harmless on USB power). No IO expander on this
// kit; the touch reset GPIO is pulsed in touch_hal_init().
extern "C" void board_init(void) {
    // Release the pad hold a power-off left behind (power.cpp latches BAT_EN
    // low with gpio_hold_en before deep sleep) — without this the HIGH write
    // below would not reach the pin after a USB pseudo-off wake.
    gpio_hold_dis((gpio_num_t)BAT_EN);
    pinMode(BAT_EN, OUTPUT);
    digitalWrite(BAT_EN, HIGH);

    Wire.begin(IIC_SDA, IIC_SCL);
}
