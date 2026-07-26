#include "board.h"
#include <Arduino.h>
#include <Wire.h>

// AMOLED-2.06 has no IO expander. LCD_RESET and TP_RESET are direct GPIOs,
// pulsed here before display_hal_init / touch_hal_init run so the CO5300
// and FT3168 are both out of reset by the time their drivers probe them.
extern "C" void board_init(void) {
    pinMode(LCD_RESET, OUTPUT);
    pinMode(TP_RESET,  OUTPUT);
    digitalWrite(LCD_RESET, LOW);
    digitalWrite(TP_RESET,  LOW);
    delay(10);
    digitalWrite(LCD_RESET, HIGH);
    digitalWrite(TP_RESET,  HIGH);
    delay(50);  // CO5300 needs ~10ms post-reset before first command

    Wire.begin(IIC_SDA, IIC_SCL);
}
