#include "../../hal/display_hal.h"
#include "board.h"
#include "io_expander.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>

// AMOLED-1.8 is fixed at 0°. No CPU rotation, no rot_buf.
// Display reset is routed through the XCA9554 IO expander (EXIO1) which
// must be initialized + released before gfx->begin() runs — main.cpp
// arranges this by calling display_hal_init() after io_expander_init().

static Arduino_DataBus* bus = nullptr;
static Arduino_SH8601*  gfx = nullptr;

void display_hal_init(void) {
    bus = new Arduino_ESP32QSPI(
        LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
    // SH8601 constructor: (bus, rst, rotation, w, h)
    gfx = new Arduino_SH8601(
        bus, GFX_NOT_DEFINED /* reset via XCA9554 */, 0,
        LCD_WIDTH, LCD_HEIGHT);
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
    // No rotation handling needed on this board.
}

// SH8601 driver doesn't strictly require even alignment in source, but the
// rounder is harmless and keeps behavior consistent with the CO5300 port.
lv_color_format_t display_hal_lv_color_format(void) {
    return LV_COLOR_FORMAT_RGB565;
}

void display_hal_round_area(int32_t* x1, int32_t* y1, int32_t* x2, int32_t* y2) {
    *x1 = *x1 & ~1;
    *y1 = *y1 & ~1;
    *x2 = *x2 | 1;
    *y2 = *y2 | 1;
}
