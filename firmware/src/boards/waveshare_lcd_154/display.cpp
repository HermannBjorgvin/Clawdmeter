#include "../../hal/display_hal.h"
#include "board.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>

// ST7789 over plain 4-wire SPI — the first non-QSPI panel in the tree, but
// Arduino_GFX abstracts the bus so the HAL surface is identical. The ST7789
// has no in-panel brightness command; brightness is a LEDC PWM duty on the
// backlight GPIO instead, which keeps the idle fade working unchanged.

static Arduino_DataBus* bus = nullptr;
static Arduino_ST7789*  gfx = nullptr;

void display_hal_init(void) {
    bus = new Arduino_ESP32SPI(LCD_DC, LCD_CS, LCD_SCLK, LCD_MOSI,
                               GFX_NOT_DEFINED /* no MISO */);
    // 240x240 window of the 240x320 GRAM: rows 0-239 at rotation 0, so
    // offsets are 0,0 (the 80-row offset only matters for rotations 2/3).
    // ips=true — this module needs color inversion (matches the
    // hardware-tested BambuHelper config's USE_ST7789_INVERT).
    gfx = new Arduino_ST7789(bus, LCD_RST, 0 /* rotation */, true /* ips */,
                             LCD_WIDTH, LCD_HEIGHT, 0, 0, 0, 80);
}

void display_hal_begin(void) {
    gfx->begin(80000000);   // 80 MHz write clock, hardware-tested on this panel
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
    // No rotation cycle on this board.
}

// ST7789 over SPI has no flush-region alignment requirement.
void display_hal_round_area(int32_t* x1, int32_t* y1, int32_t* x2, int32_t* y2) {
    (void)x1; (void)y1; (void)x2; (void)y2;
}
