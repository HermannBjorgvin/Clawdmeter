#include "../../hal/display_hal.h"
#include "board.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>

// TODO: pick the right driver class from Arduino_GFX_Library (e.g.
// Arduino_CO5300, Arduino_SH8601, Arduino_NV3041A). Most QSPI AMOLED
// panels are supported in the upstream library — check the panel's
// chip and grep the library include path.

static Arduino_DataBus* bus = nullptr;
// static Arduino_<YOUR_PANEL>* gfx = nullptr;

void display_hal_init(void) {
    bus = new Arduino_ESP32QSPI(
        LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
    // gfx = new Arduino_<YOUR_PANEL>(bus, LCD_RESET, 0, LCD_WIDTH, LCD_HEIGHT, ...);
}

void display_hal_begin(void) {
    // gfx->begin();
    // gfx->fillScreen(0x0000);
    // gfx->setBrightness(200);
}

void display_hal_set_brightness(uint8_t level) {
    (void)level;
    // gfx->setBrightness(level);
}

void display_hal_fill_screen(uint16_t color) {
    (void)color;
    // gfx->fillScreen(color);
}

void display_hal_draw_bitmap(int32_t x, int32_t y, int32_t w, int32_t h,
                             const uint16_t* pixels) {
    (void)x; (void)y; (void)w; (void)h; (void)pixels;
    // gfx->draw16bitRGBBitmap(x, y, (uint16_t*)pixels, w, h);
    //
    // If your panel needs CPU rotation (no native MADCTL rotate), set
    // BOARD_HAS_ROTATION=1 in board.h, allocate a rotation strip buffer
    // in display_hal_begin(), and transform (x, y, w, h) + pixels here.
    // See boards/waveshare_amoled_216/display.cpp for a worked example.
}

void display_hal_tick(void) {
    // Only needed for boards that animate the brightness ramp during a
    // CPU-rotation transition (see the 2.16 reference port).
}

void display_hal_round_area(int32_t* x1, int32_t* y1, int32_t* x2, int32_t* y2) {
    // Most QSPI AMOLED drivers expect even-aligned flush regions. Harmless
    // to apply on panels that don't strictly require it.
    *x1 = *x1 & ~1;
    *y1 = *y1 & ~1;
    *x2 = *x2 | 1;
    *y2 = *y2 | 1;
}
