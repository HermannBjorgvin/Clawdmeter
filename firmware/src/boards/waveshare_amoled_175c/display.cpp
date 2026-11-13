#include "../../hal/display_hal.h"
#include "board.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>

static Arduino_DataBus* bus = nullptr;
static Arduino_CO5300* gfx = nullptr;

void display_hal_init(void) {
    bus = new Arduino_ESP32QSPI(
        LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
    gfx = new Arduino_CO5300(
        bus, LCD_RESET, 0, LCD_WIDTH, LCD_HEIGHT, CO5300_COL_OFFSET, 0, 0, 0);
}

void display_hal_begin(void) {
    gfx->begin();
    gfx->fillScreen(0x0000);
    gfx->setBrightness(200);
}

void display_hal_set_brightness(uint8_t level) {
    if (gfx) gfx->setBrightness(level);
}

void display_hal_draw_bitmap(int32_t x, int32_t y, int32_t w, int32_t h,
                             const uint16_t* pixels) {
    if (gfx) gfx->draw16bitRGBBitmap(x, y, (uint16_t*)pixels, w, h);
}

void display_hal_tick(void) {}

void display_hal_round_area(int32_t* x1, int32_t* y1, int32_t* x2, int32_t* y2) {
    *x1 &= ~1;
    *y1 &= ~1;
    *x2 |= 1;
    *y2 |= 1;
}
