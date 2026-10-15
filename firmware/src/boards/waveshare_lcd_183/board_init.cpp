#include "board.h"
#include <Arduino.h>
#include <Wire.h>

// No IO expander on this board: the LCD reset is driven by the GFX driver
// (LCD_RST is handed to Arduino_ST7789, which pulses it in begin()), and the
// touch reset is a plain GPIO we release here before anything probes I2C.

extern "C" void board_init(void) {
    Wire.begin(IIC_SDA, IIC_SCL);

    // Bring the CST816 touch controller out of reset (active LOW).
    pinMode(TP_RST, OUTPUT);
    digitalWrite(TP_RST, LOW);
    delay(10);
    digitalWrite(TP_RST, HIGH);
    delay(50);
}
