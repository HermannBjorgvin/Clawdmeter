#include "../../hal/display_hal.h"
#include "board.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>

// ST7789V2 over plain 4-wire SPI — same shape as the LCD-1.54 port, two
// differences:
//
//  * Row offset. The controller's GRAM is 240x320; a 240x280 panel exposes
//    rows 20..299 of it, so row_offset1 = 20 (and row_offset2 = 20 for the
//    flipped rotations). Get this wrong and the image sits 20 px off with a
//    band of garbage at one edge. For a 240x240 panel use 0/80 instead;
//    for a full 240x320, 0/0.
//  * SPI clock. 40 MHz, not the kit's 80 MHz — this port is wired with
//    jumpers, where 80 MHz is where garbage pixels start. On a soldered
//    board you can push it back up.
//
// ips=true inverts colors, which is what ST7789V2 IPS modules want. If your
// panel comes out photo-negative, flip it to false — that is the whole fix.

#define LCD_SPI_HZ  40000000

static Arduino_DataBus* bus = nullptr;
static Arduino_ST7789*  gfx = nullptr;

void display_hal_init(void) {
    bus = new Arduino_ESP32SPI(LCD_DC, LCD_CS, LCD_SCLK, LCD_MOSI,
                               GFX_NOT_DEFINED /* no MISO */);
    gfx = new Arduino_ST7789(bus, LCD_RST, 0 /* rotation */, true /* ips */,
                             LCD_WIDTH, LCD_HEIGHT,
                             0 /* col_offset1 */, 20 /* row_offset1 */,
                             0 /* col_offset2 */, 20 /* row_offset2 */);
}

void display_hal_begin(void) {
    gfx->begin(LCD_SPI_HZ);
    gfx->fillScreen(0x0000);
    ledcAttach(LCD_BL, 5000 /* Hz */, 8 /* bits */);
    ledcWrite(LCD_BL, 200);
}

void display_hal_set_brightness(uint8_t level) {
    ledcWrite(LCD_BL, level);
}

void display_hal_fill_screen(uint16_t color) {
    if (gfx) gfx->fillScreen(color);
}

void display_hal_draw_bitmap(int32_t x, int32_t y, int32_t w, int32_t h,
                             const uint16_t* pixels) {
    if (gfx) gfx->draw16bitRGBBitmap(x, y, (uint16_t*)pixels, w, h);
}

void display_hal_tick(void) {
    // No rotation on this board.
}

// ST7789 over SPI has no flush-region alignment requirement.
void display_hal_round_area(int32_t* x1, int32_t* y1, int32_t* x2, int32_t* y2) {
    (void)x1; (void)y1; (void)x2; (void)y2;
}
