#include "board.h"
#include <Arduino.h>
#include <Wire.h>

// No IO expander on this board — LCD_RST is driven by the GFX driver, and the
// touch controller just needs its own reset released before it's probed.

extern "C" void board_init(void) {
    Wire.begin(IIC_SDA, IIC_SCL);

    // Release the CST816 from reset (active LOW) so it ACKs on I2C.
    pinMode(TP_RST, OUTPUT);
    digitalWrite(TP_RST, LOW);
    delay(10);
    digitalWrite(TP_RST, HIGH);
    delay(50);
}
