#include "io_expander.h"
#include "board.h"
#include <Arduino.h>
#include <Wire.h>

// TCA9554: 0x01 = output latch, 0x03 = config (1 = input). Power-on state is
// all inputs with the latch at 0xFF, so the latch is written before the
// config. EXIO5-7 carry the IMU and RTC interrupts and stay inputs (the
// Waveshare demo drives all eight).
//
// Without the expander the LCD/touch resets never pulse and the ST7701S CS
// never goes low, so the panel ignores its init: backlight on, black screen.
// The probe retries once and failed writes are logged so that shows up.

#define IOX_REG_OUTPUT  0x01
#define IOX_REG_CONFIG  0x03
#define IOX_INPUTS      0x70   // EXIO5, EXIO6, EXIO7

// Resets released, LCD and SD chip selects deselected, buzzer off.
#define IOX_LATCH_INIT  (uint8_t)((1u << IOX_BIT_LCD_RST) | (1u << IOX_BIT_TP_RST) | \
                                  (1u << IOX_BIT_LCD_CS)  | (1u << IOX_BIT_SD_CS))

static bool    expander_ok  = false;
static uint8_t output_latch = IOX_LATCH_INIT;

static bool iox_probe(void) {
    Wire.beginTransmission(IO_EXPANDER_ADDR);
    return Wire.endTransmission() == 0;
}

static void iox_write(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(IO_EXPANDER_ADDR);
    Wire.write(reg);
    Wire.write(val);
    uint8_t err = Wire.endTransmission();
    if (err != 0) {
        Serial.printf("LCD-2.1 IO expander write reg 0x%02X=0x%02X failed (%u)\n",
                      reg, val, err);
    }
}

void io_expander_init(void) {
    if (!iox_probe()) {
        delay(10);
        if (!iox_probe()) {
            Serial.println("LCD-2.1 IO expander not found (0x20): panel and touch stay in reset");
            return;
        }
    }
    expander_ok = true;
    iox_write(IOX_REG_OUTPUT, output_latch);
    iox_write(IOX_REG_CONFIG, IOX_INPUTS);
    Serial.println("LCD-2.1 IO expander init OK");
}

void io_expander_set(uint8_t bit, bool high) {
    if (!expander_ok) return;
    if (high) output_latch |= (uint8_t)(1u << bit);
    else      output_latch &= (uint8_t)~(1u << bit);
    iox_write(IOX_REG_OUTPUT, output_latch);
}
