#include "ft6336.h"
#include "board.h"
#include <Arduino.h>
#include <Wire.h>

static bool      ok   = false;
static TouchZone zone = TZ_NONE;
static uint16_t  tx = 0, ty = 0;
static uint32_t  last_ms = 0;
static uint32_t  last_probe_ms = 0;
static uint8_t   failed_reads = 0;

#define POLL_MS          15
#define PROBE_RETRY_MS   1000
// Failed reads a held zone survives (~75 ms) before it reads as released:
// one glitch is no phantom tap, a dead controller can't hold a button down.
#define MAX_FAILED_READS 5

static bool probe(void) {
    Wire.beginTransmission(TOUCH_ADDR);
    Wire.write(0xA4);   // G_MODE: polling (INT stays low while touched)
    Wire.write(0x00);
    return Wire.endTransmission() == 0;
}

bool ft6336_init(void) {
    pinMode(TOUCH_INT_GPIO, INPUT);
    ok = probe();
    last_probe_ms = millis();
    Serial.printf("Core2: FT6336U touch %s\n", ok ? "OK" : "not found yet, retrying");
    return ok;
}

static TouchZone strip_zone(uint16_t x) {
    if (x < LCD_WIDTH / 3)     return TZ_BTN_A;
    if (x < LCD_WIDTH * 2 / 3) return TZ_BTN_B;
    return TZ_BTN_C;
}

void ft6336_poll(void) {
    const uint32_t now = millis();
    if (!ok) {
        // The controller comes out of the power chip's reset late on some
        // boots: keep probing, or the board has no input and can't pair.
        if (now - last_probe_ms < PROBE_RETRY_MS) return;
        last_probe_ms = now;
        if (!probe()) return;
        ok = true;
        Serial.println("Core2: FT6336U touch OK (late)");
    }
    if (now - last_ms < POLL_MS) return;
    last_ms = now;

    Wire.beginTransmission(TOUCH_ADDR);
    Wire.write(0x02);
    if (Wire.endTransmission(false) != 0 || Wire.requestFrom((uint8_t)TOUCH_ADDR, (uint8_t)5) != 5) {
        if (++failed_reads >= MAX_FAILED_READS) zone = TZ_NONE;
        return;
    }
    failed_reads = 0;
    uint8_t n  = Wire.read() & 0x0F;
    uint8_t xh = Wire.read(), xl = Wire.read(), yh = Wire.read(), yl = Wire.read();
    if (n == 0 || n > 2) { zone = TZ_NONE; return; }

    uint16_t x = ((uint16_t)(xh & 0x0F) << 8) | xl;
    uint16_t y = ((uint16_t)(yh & 0x0F) << 8) | yl;
    if (x >= LCD_WIDTH) x = LCD_WIDTH - 1;

    if (zone == TZ_NONE) zone = (y >= TOUCH_STRIP_Y) ? strip_zone(x) : TZ_SCREEN;
    if (zone == TZ_SCREEN) {
        tx = x;
        ty = (y >= LCD_HEIGHT) ? LCD_HEIGHT - 1 : y;
    }
}

TouchZone ft6336_zone(void) { return zone; }
uint16_t  ft6336_x(void)    { return tx; }
uint16_t  ft6336_y(void)    { return ty; }
