#include "board.h"
#include <Arduino.h>

// Raise the panel rail before anything talks to the ST7789, and park the
// parallel bus's read strobe high. No I2C devices on this board.
extern "C" void board_init(void) {
    pinMode(LCD_POWER_ON, OUTPUT);
    digitalWrite(LCD_POWER_ON, HIGH);
    pinMode(LCD_RD, OUTPUT);
    digitalWrite(LCD_RD, HIGH);
}
