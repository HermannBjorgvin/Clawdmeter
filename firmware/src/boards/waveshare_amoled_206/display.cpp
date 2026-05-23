#include "../../hal/display_hal.h"
#include "board.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>

// AMOLED-2.06 is fixed at 0° (watch enclosure). No CPU rotation, no rot_buf.
// LCD_RESET is a direct GPIO; board_init() pulses it before this driver runs,
// so we hand the GFX driver GFX_NOT_DEFINED.

static Arduino_DataBus* bus = nullptr;
static Arduino_CO5300*  gfx = nullptr;

void display_hal_init(void) {
    bus = new Arduino_ESP32QSPI(
        LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
    // CO5300 constructor: (bus, rst, rotation, w, h, col_off1, row_off1, col_off2, row_off2)
    // col_offset1 = 22: the 2.06" panel exposes its 410-wide viewport at a 22-pixel
    // horizontal offset inside the controller's internal 454-wide RAM. Without this,
    // writes land at the wrong columns and a vertical strip of stale content shows
    // through on the right. Value matches Waveshare's reference Mylibrary.
    gfx = new Arduino_CO5300(
        bus, GFX_NOT_DEFINED /* reset pulsed in board_init */, 0,
        LCD_WIDTH, LCD_HEIGHT, 23, 0, 0, 0);
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

// CO5300 requires even-aligned flush regions.
void display_hal_round_area(int32_t* x1, int32_t* y1, int32_t* x2, int32_t* y2) {
    *x1 = *x1 & ~1;
    *y1 = *y1 & ~1;
    *x2 = *x2 | 1;
    *y2 = *y2 | 1;
}
