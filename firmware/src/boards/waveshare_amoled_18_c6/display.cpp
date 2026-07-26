#include "../../hal/display_hal.h"
#include "board.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>

// Same SH8601 368x448 panel as the S3 AMOLED-1.8, so the stock Arduino_GFX
// SH8601 init is sufficient — no vendor-register patch like the C6 2.16
// needs. Display power is gated by the TCA9554 (P4), brought up in
// board_init() before this runs; reset is the panel's internal POR, so the
// GFX driver gets GFX_NOT_DEFINED. Rotation is disabled (no PSRAM headroom).

static Arduino_DataBus* bus = nullptr;
static Arduino_SH8601*  gfx = nullptr;

void display_hal_init(void) {
    bus = new Arduino_ESP32QSPI(
        LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
    gfx = new Arduino_SH8601(
        bus, GFX_NOT_DEFINED, 0, LCD_WIDTH, LCD_HEIGHT);
}

void display_hal_begin(void) {
    gfx->begin();
    gfx->fillScreen(0x0000);
    gfx->setBrightness(200);
}

void display_hal_set_brightness(uint8_t level) {
    if (gfx) gfx->setBrightness(level);
}

void display_hal_fill_screen(uint16_t color) {
    if (gfx) gfx->fillScreen(color);
}

void display_hal_draw_bitmap(int32_t x, int32_t y, int32_t w, int32_t h,
                             const uint16_t* pixels) {
    if (gfx) gfx->draw16bitRGBBitmap(x, y, (uint16_t*)pixels, w, h);
}

void display_hal_tick(void) {
    // No rotation cycle on this board.
}

// Even-alignment rounder — harmless on SH8601, kept for consistency with the
// other AMOLED ports.
void display_hal_round_area(int32_t* x1, int32_t* y1, int32_t* x2, int32_t* y2) {
    *x1 = *x1 & ~1;
    *y1 = *y1 & ~1;
    *x2 = *x2 | 1;
    *y2 = *y2 | 1;
}
