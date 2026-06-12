#include "board.h"
#include <Arduino.h>

// Single LilyGo_AMOLED instance shared by all HAL files for this board.
LilyGo_AMOLED amoled;

extern "C" void board_init(void) {
    // Assert PMIC enable before I2C/display power-up.
    pinMode(PMIC_EN_PIN, OUTPUT);
    digitalWrite(PMIC_EN_PIN, HIGH);

    // beginAMOLED_241(disable_sd=true): no SD slot on T4-S3, skip SPI init.
    if (!amoled.beginAMOLED_241(true)) {
        Serial.println("LilyGo AMOLED init FAILED");
    } else {
        Serial.printf("LilyGo T4-S3 init OK (%dx%d)\n",
                      amoled.width(), amoled.height());
    }
}
