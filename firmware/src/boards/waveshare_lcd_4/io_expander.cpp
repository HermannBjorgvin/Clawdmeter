#include "io_expander.h"
#include "board.h"
#include <Arduino.h>
#include <Wire.h>

// Two register maps are in the wild on this kit:
//   TCA9554:     0x01 = output, 0x02 = polarity, 0x03 = config
//   CH32V003:    0x02 = output, 0x03 = config
// The hardware-tested lcd4 fork programmed 0x02=0xFF / 0x03=0x3A. We also
// write 0x01 so a real TCA9554 gets its output latch set. Config 0x3A makes
// bits 0/2/6/7 outputs (backlight is bit 2).

#define IOX_REG_OUTPUT_TCA   0x01
#define IOX_REG_OUTPUT_CH32  0x02
#define IOX_REG_CONFIG       0x03
#define IOX_CONFIG           0x3A
#define IOX_OUTPUT_ON        0xFF

static uint8_t expander_addr = 0;
static uint8_t output_latch  = IOX_OUTPUT_ON;

static bool iox_probe(uint8_t addr) {
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
}

static void iox_write(uint8_t reg, uint8_t val) {
    if (!expander_addr) return;
    Wire.beginTransmission(expander_addr);
    Wire.write(reg);
    Wire.write(val);
    Wire.endTransmission();
}

static void iox_commit_output(void) {
    iox_write(IOX_REG_OUTPUT_TCA, output_latch);
    iox_write(IOX_REG_OUTPUT_CH32, output_latch);
}

void io_expander_init(void) {
    if (iox_probe(IO_EXPANDER_ADDR)) {
        expander_addr = IO_EXPANDER_ADDR;
    } else if (iox_probe(IO_EXPANDER_ADDR_ALT)) {
        expander_addr = IO_EXPANDER_ADDR_ALT;
    } else {
        Serial.println("LCD-4 IO expander not found (0x24/0x20)");
        return;
    }

    output_latch = IOX_OUTPUT_ON;
    iox_commit_output();
    iox_write(IOX_REG_CONFIG, IOX_CONFIG);
    Serial.printf("LCD-4 IO expander init OK (addr 0x%02X)\n", expander_addr);
}

void io_expander_set_backlight(bool on) {
    if (on) output_latch |= (1u << IOX_PIN_BACKLIGHT);
    else    output_latch &= ~(1u << IOX_PIN_BACKLIGHT);
    iox_commit_output();
}

uint8_t io_expander_addr(void) { return expander_addr; }
