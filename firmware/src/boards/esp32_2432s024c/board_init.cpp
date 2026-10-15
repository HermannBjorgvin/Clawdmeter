#include "board.h"

#include <Arduino.h>
#include <Wire.h>

extern "C" void board_init(void) {
    Wire.begin(IIC_SDA, IIC_SCL);
    Wire.setClock(400000);

    pinMode(TP_RESET, OUTPUT);
    digitalWrite(TP_RESET, LOW);
    delay(10);
    digitalWrite(TP_RESET, HIGH);
    delay(50);
}
