#include "board.h"
#include <Arduino.h>
#include <Wire.h>

// No IO expander on this board — touch reset is a direct GPIO that must be
// pulsed before touch_hal_init() probes the controller. The LCD reset (GPIO3)
// is handled inside display_hal_init() by the Arduino_ST77916 driver itself.
extern "C" void board_init(void) {
    Wire.begin(IIC_SDA, IIC_SCL);

    pinMode(TP_RST, OUTPUT);
    digitalWrite(TP_RST, LOW);
    delay(30);
    digitalWrite(TP_RST, HIGH);
    delay(50);
}
