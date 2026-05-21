#include "../../hal/display_hal.h"
#include "board.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>

// RM690B0 native portrait — no rotation buffer needed.

static Arduino_DataBus*  bus = nullptr;
static Arduino_RM690B0*  gfx = nullptr;

void display_hal_init(void) {
    bus = new Arduino_ESP32QSPI(
        LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
    // The visible viewport is the middle 450 columns of a 482-wide controller
    // RAM, so every transfer needs col_offset = LCD_COL_OFFSET on both
    // sides. Row offset is 0.
    gfx = new Arduino_RM690B0(
        bus, LCD_RESET, 0 /* rotation */,
        LCD_WIDTH, LCD_HEIGHT,
        LCD_COL_OFFSET, 0, LCD_COL_OFFSET, 0);
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
    if (!gfx) return;
    gfx->draw16bitRGBBitmap(x, y, (uint16_t*)pixels, w, h);
}

void display_hal_tick(void) {
    // No-op: no IMU-driven rotation on this board.
}

// RM690B0 expects even-aligned flush regions like the other QSPI AMOLEDs.
void display_hal_round_area(int32_t* x1, int32_t* y1, int32_t* x2, int32_t* y2) {
    *x1 = *x1 & ~1;
    *y1 = *y1 & ~1;
    *x2 = *x2 | 1;
    *y2 = *y2 | 1;
}
