#include "board.h"
#include "io_expander.h"
#include <Arduino.h>
#include <Wire.h>

// Clear the I2C bus, then shared I2C, then the expander, then pulse the LCD and touch resets (both
// are expander pins). Timings are the Waveshare demo's. All of this MUST
// happen before display_hal_begin() or the ST7701S and touch controller stay
// in reset.
//
// 2.8C (BOARD_LCD28C): the GT911 latches its I2C address from INT at the
// rising edge of reset, INT held low giving 0x5D. Datasheet order: assert
// reset first, so the running controller isn't driving INT when GPIO 16
// pulls it low (the Waveshare demo drives INT first and contends with it),
// then release both resets together, hold INT low >= 5 ms, release it and
// wait 50 ms before I2C. About 70 ms in all (the demo waits 350 ms).
extern "C" void board_init(void) {
    // The expander keeps its latch through an ESP32 reset and may be stuck
    // mid-transaction (io_expander_recover_bus()).
    if (!io_expander_recover_bus()) Serial.println(BOARD_LOG_TAG " I2C: SDA/SCL held low after bus recovery");
    Wire.begin(IIC_SDA, IIC_SCL);
    io_expander_init();

    io_expander_set(IOX_BIT_LCD_RST, false);
    io_expander_set(IOX_BIT_TP_RST, false);
#ifdef BOARD_LCD28C
    pinMode(TP_INT, OUTPUT);
    digitalWrite(TP_INT, LOW);
#endif
    delay(10);
    io_expander_set(IOX_BIT_LCD_RST, true);
    io_expander_set(IOX_BIT_TP_RST, true);
#ifdef BOARD_LCD28C
    delay(10);   // INT held low >= 5 ms after reset rises: address 0x5D
    pinMode(TP_INT, INPUT);
#endif
    delay(50);   // 2.8C: the GT911 needs >= 50 ms after INT is released before I2C
}
