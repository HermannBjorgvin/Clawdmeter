#include "../../hal/touch_hal.h"
#include "board.h"
#include <Arduino.h>
#include <SPI.h>

// XPT2046 resistive touch — the first non-capacitive panel in the tree.
//
// Vendored as a ~40-line SPI reader rather than pulling a driver in, matching
// the house posture on touch controllers (see docs/porting/hal-contract.md).
// It sits on its own SPI peripheral so it never contends with the 40 MHz
// display bus; the pins aren't native VSPI pins, so they route through the
// GPIO matrix, and the bus is clocked at 2 MHz (the XPT2046's own limit).
//
// Resistive differs from every capacitive port here in two ways that matter:
//
//  1. There is no controller-side calibration. The chip returns raw 12-bit
//     ADC counts and the usable range is a property of the individual panel,
//     so TP_RAW_* in board.h are defaults, not facts. Build with -DTOUCH_DEBUG
//     to stream raw samples over serial and retune.
//  2. There is no "finger present" bit. Pressure is derived from two extra
//     conversions and gated on TP_PRESSURE_MIN, otherwise release transients
//     land as phantom taps in the corner.

static SPIClass    touch_spi(VSPI);
static SPISettings touch_cfg(TP_SPI_SPEED, MSBFIRST, SPI_MODE0);

// XPT2046 control bytes: start | A2A1A0 | 12-bit | differential | power-down.
static const uint8_t CMD_X  = 0xD0;
static const uint8_t CMD_Y  = 0x90;
static const uint8_t CMD_Z1 = 0xB0;
static const uint8_t CMD_Z2 = 0xC0;

// One conversion. The result arrives in the 16 clocks after the command with
// a one-clock delay, hence the >> 3 to land the 12 significant bits.
static uint16_t xpt_read(uint8_t cmd) {
    touch_spi.transfer(cmd);
    const uint16_t hi = touch_spi.transfer(0x00);
    const uint16_t lo = touch_spi.transfer(0x00);
    return (uint16_t)(((hi << 8) | lo) >> 3);
}

static uint16_t median3(uint16_t a, uint16_t b, uint16_t c) {
    if (a > b) { const uint16_t t = a; a = b; b = t; }
    if (b > c) { const uint16_t t = b; b = c; c = t; }
    if (a > b) { const uint16_t t = a; a = b; b = t; }
    return b;
}

// Raw ADC count → 0..4095 normalised position along one axis.
static uint32_t normalise(uint16_t raw, uint16_t rmin, uint16_t rmax) {
    if (raw <= rmin) return 0;
    if (raw >= rmax) return 4095;
    return ((uint32_t)(raw - rmin) * 4095u) / (uint32_t)(rmax - rmin);
}

void touch_hal_init(void) {
    pinMode(TP_IRQ, INPUT);
    pinMode(TP_CS, OUTPUT);
    digitalWrite(TP_CS, HIGH);
    touch_spi.begin(TP_CLK, TP_MISO, TP_MOSI, TP_CS);

    // One throwaway conversion leaves the chip in a known powered-down state
    // and confirms the bus is wired; the XPT2046 has no ID register to probe.
    digitalWrite(TP_CS, LOW);
    touch_spi.beginTransaction(touch_cfg);
    (void)xpt_read(CMD_Z1);
    touch_spi.endTransaction();
    digitalWrite(TP_CS, HIGH);

    Serial.printf("Touch XPT2046 on SPI (CLK %d, CS %d, IRQ %d) @ %d kHz\n",
                  TP_CLK, TP_CS, TP_IRQ, TP_SPI_SPEED / 1000);
}

void touch_hal_read(uint16_t* x, uint16_t* y, bool* pressed) {
    // PENIRQ is the cheap gate: HIGH means nothing is touching the panel, and
    // we skip the SPI traffic entirely. This is what keeps the HAL's <5 ms
    // budget comfortable — the loop only pays for reads while a finger is down.
    if (digitalRead(TP_IRQ) == HIGH) {
        *pressed = false;
        return;
    }

    digitalWrite(TP_CS, LOW);
    touch_spi.beginTransaction(touch_cfg);

    const uint16_t z1 = xpt_read(CMD_Z1);
    const uint16_t z2 = xpt_read(CMD_Z2);

    uint16_t xs[3], ys[3];
    for (int i = 0; i < 3; ++i) {
        ys[i] = xpt_read(CMD_Y);
        xs[i] = xpt_read(CMD_X);
    }

    touch_spi.endTransaction();
    digitalWrite(TP_CS, HIGH);

    // Pressure rises with z1 and falls with z2; below the floor it's a
    // release transient or plain noise.
    const int32_t pressure = (int32_t)z1 + (4095 - (int32_t)z2);
    if (pressure < TP_PRESSURE_MIN) {
        *pressed = false;
        return;
    }

    const uint16_t raw_x = median3(xs[0], xs[1], xs[2]);
    const uint16_t raw_y = median3(ys[0], ys[1], ys[2]);

    uint32_t a = normalise(raw_x, TP_RAW_X_MIN, TP_RAW_X_MAX);
    uint32_t b = normalise(raw_y, TP_RAW_Y_MIN, TP_RAW_Y_MAX);

#if TP_SWAP_XY
    { const uint32_t t = a; a = b; b = t; }
#endif
#if TP_INVERT_X
    a = 4095u - a;
#endif
#if TP_INVERT_Y
    b = 4095u - b;
#endif

    *x = (uint16_t)((a * (LCD_WIDTH - 1)) / 4095u);
    *y = (uint16_t)((b * (LCD_HEIGHT - 1)) / 4095u);
    *pressed = true;

#ifdef TOUCH_DEBUG
    // Retuning recipe: press each corner in turn and read off the extremes of
    // raw x/y, then set TP_RAW_* in board.h. If the reported point moves the
    // wrong way, flip TP_SWAP_XY / TP_INVERT_X / TP_INVERT_Y instead.
    static uint32_t last_log_ms = 0;
    if (millis() - last_log_ms > 200) {
        last_log_ms = millis();
        Serial.printf("touch raw=(%4u,%4u) z=%4ld -> (%3u,%3u)\n",
                      raw_x, raw_y, (long)pressure, *x, *y);
    }
#endif
}
