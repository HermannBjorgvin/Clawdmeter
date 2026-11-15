#include "board.h"
#include "io_expander.h"
#include <Arduino.h>
#include <Wire.h>

// Shared I2C, then the expander, then pulse the LCD and touch resets (both
// are expander pins). All of this MUST happen before display_hal_begin() or
// the ST7701S and GT911 stay in reset.
//
// The GT911 latches its I2C address from INT at the rising edge of reset:
// INT held low gives 0x5D. The sequence and timings are the Waveshare demo's
// (touch_gt911_reset); INT is released to an input afterwards.
extern "C" void board_init(void) {
    Wire.begin(IIC_SDA, IIC_SCL);
    io_expander_init();

    pinMode(TP_INT, OUTPUT);
    digitalWrite(TP_INT, LOW);
    io_expander_set(IOX_BIT_LCD_RST, false);
    io_expander_set(IOX_BIT_TP_RST, false);
    delay(10);
    io_expander_set(IOX_BIT_LCD_RST, true);
    delay(140);
    io_expander_set(IOX_BIT_TP_RST, true);
    delay(50);
    pinMode(TP_INT, INPUT);
    delay(50);   // GT911 needs >= 50 ms after INT is released before I2C
}
