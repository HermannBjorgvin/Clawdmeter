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

// The TCA9554 has no reset line and keeps its latch through an ESP32 reset,
// so a reset in the middle of an I2C transaction can leave it (or another
// chip on the bus) holding SDA low, and EXIO8 still driving the buzzer.
// Clock the bus free before Wire.begin(): up to 9 SCL pulses let a slave
// finish the byte it's stuck in, then a STOP. Same as the LCD-4's.
bool io_expander_recover_bus(void) {
    pinMode(IIC_SDA, INPUT_PULLUP);
    pinMode(IIC_SCL, OUTPUT_OPEN_DRAIN);
    digitalWrite(IIC_SCL, HIGH);
    delayMicroseconds(10);
    for (int i = 0; i < 9 && digitalRead(IIC_SDA) == LOW; ++i) {
        digitalWrite(IIC_SCL, LOW);
        delayMicroseconds(10);
        digitalWrite(IIC_SCL, HIGH);
        delayMicroseconds(10);
    }
    // STOP condition: SDA low → high while SCL is high.
    pinMode(IIC_SDA, OUTPUT_OPEN_DRAIN);
    digitalWrite(IIC_SDA, LOW);
    delayMicroseconds(10);
    digitalWrite(IIC_SCL, HIGH);
    delayMicroseconds(10);
    digitalWrite(IIC_SDA, HIGH);
    delayMicroseconds(10);
    pinMode(IIC_SDA, INPUT_PULLUP);
    pinMode(IIC_SCL, INPUT_PULLUP);
    delay(2);
    return digitalRead(IIC_SDA) == HIGH && digitalRead(IIC_SCL) == HIGH;
}

static bool iox_probe(void) {
    Wire.beginTransmission(IO_EXPANDER_ADDR);
    return Wire.endTransmission() == 0;
}

// Writes run from loop() too (buzzer edges, and a failed "off" retried every
// 200 ms), so a failing expander logs its first failure and its recovery,
// not every attempt.
static uint32_t write_failures = 0;

static bool iox_write(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(IO_EXPANDER_ADDR);
    Wire.write(reg);
    Wire.write(val);
    uint8_t err = Wire.endTransmission();
    if (err != 0) {
        if (write_failures++ == 0) {
            Serial.printf(BOARD_LOG_TAG " IO expander write reg 0x%02X=0x%02X failed (%u)\n",
                          reg, val, err);
        }
        return false;
    }
    if (write_failures > 0) {
        Serial.printf(BOARD_LOG_TAG " IO expander writes OK again after %lu failed\n",
                      (unsigned long)write_failures);
        write_failures = 0;
    }
    return true;
}

void io_expander_init(void) {
    if (!iox_probe()) {
        delay(10);
        if (!iox_probe()) {
            Serial.println(BOARD_LOG_TAG " IO expander not found (0x20): panel and touch stay in reset, buzzer can't be switched");
            return;
        }
    }
    expander_ok = true;
    iox_write(IOX_REG_OUTPUT, output_latch);
    iox_write(IOX_REG_CONFIG, IOX_INPUTS);
    Serial.println(BOARD_LOG_TAG " IO expander init OK");
}

// No expander means nothing to drive (the pins stay at their power-on state),
// so that reports success: a buzzer "off" isn't retried forever.
// The shadow latch changes only when the write lands, so it always matches
// the chip: a failed buzzer "on" can't ride along on a later write for
// another pin.
bool io_expander_set(uint8_t bit, bool high) {
    if (!expander_ok) return true;
    const uint8_t next = high ? (uint8_t)(output_latch | (1u << bit))
                              : (uint8_t)(output_latch & ~(1u << bit));
    if (!iox_write(IOX_REG_OUTPUT, next)) return false;
    output_latch = next;
    return true;
}
