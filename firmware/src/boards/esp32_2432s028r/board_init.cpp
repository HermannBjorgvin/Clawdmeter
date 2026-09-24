#include "board.h"

#include <Arduino.h>

// Nothing on this board gates the panel: LCD reset is tied to EN, touch sits
// on its own SPI bus, and there is no I2C peripheral at all (so no Wire.begin
// here, unlike every other port). All board_init() has to do is park the
// backlight off, so the panel stays dark through ILI9341 init instead of
// flashing whatever noise is in GRAM at power-on.
extern "C" void board_init(void) {
    pinMode(LCD_BL, OUTPUT);
    digitalWrite(LCD_BL, LOW);
}
