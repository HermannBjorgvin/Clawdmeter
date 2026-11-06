#include "board.h"

#include <Arduino.h>

// Nothing gates the panel: Arduino_GFX pulses LCD_RST itself, and there is no
// I2C peripheral. Park the backlight off so the panel stays dark through
// ST7789 init instead of flashing whatever noise is in GRAM at power-on.
extern "C" void board_init(void) {
    pinMode(LCD_BL, OUTPUT);
    digitalWrite(LCD_BL, LOW);
}
