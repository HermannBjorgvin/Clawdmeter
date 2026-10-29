#include "board.h"
#include "io_expander.h"
#include <Arduino.h>
#include <Wire.h>

// Shared I2C, then the expander, then pulse the LCD and touch resets (both
// are expander pins). Timings are the Waveshare demo's. All of this MUST
// happen before display_hal_begin() or the ST7701S and CST820 stay in reset.
extern "C" void board_init(void) {
    Wire.begin(IIC_SDA, IIC_SCL);
    io_expander_init();

    io_expander_set(IOX_BIT_LCD_RST, false);
    io_expander_set(IOX_BIT_TP_RST, false);
    delay(10);
    io_expander_set(IOX_BIT_LCD_RST, true);
    io_expander_set(IOX_BIT_TP_RST, true);
    delay(50);
}
