#include "../../hal/display_hal.h"
#include "board.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>

// ST7789P3 over plain 4-wire SPI. The P3 answers to the standard ST7789 init
// (M5GFX drives it with its generic ST7789 panel class too). Brightness is
// LEDC PWM on the backlight GPIO; the panel rail itself is PM1-gated in
// board_init(), which runs first.

static Arduino_DataBus* bus = nullptr;
static Arduino_ST7789*  gfx = nullptr;

void display_hal_init(void) {
    bus = new Arduino_ESP32SPI(LCD_DC, LCD_CS, LCD_SCLK, LCD_MOSI,
                               GFX_NOT_DEFINED /* no MISO */);
    // 135x240 window of the 240x320 GRAM: column offset 52 (53 on the far
    // edge), row offset 40 — the classic ST7789 135x240 mapping, matching
    // M5GFX's offset_x=52 / offset_y=40 for this board. IPS → inversion on.
    gfx = new Arduino_ST7789(bus, LCD_RST, 0 /* rotation */, true /* ips */,
                             LCD_WIDTH, LCD_HEIGHT, 52, 40, 53, 40);
}

void display_hal_begin(void) {
    gfx->begin(40000000);   // 40 MHz — M5GFX's write clock for this panel
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
