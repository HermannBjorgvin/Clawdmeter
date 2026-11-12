#include "../../hal/display_hal.h"
#include "board.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>

// ST7789 over an 8-bit i80 parallel bus (Arduino_ESP32LCD8). The panel has no
// brightness command; brightness is a LEDC PWM duty on the backlight GPIO, so
// the idle fade works unchanged.

static Arduino_DataBus* bus = nullptr;
static Arduino_ST7789*  gfx = nullptr;

void display_hal_init(void) {
    bus = new Arduino_ESP32LCD8(
        LCD_DC, LCD_CS, LCD_WR, LCD_RD,
        LCD_D0, LCD_D1, LCD_D2, LCD_D3, LCD_D4, LCD_D5, LCD_D6, LCD_D7);
    gfx = new Arduino_ST7789(
        bus, LCD_RST, 0 /* rotation */, true /* ips */,
        LCD_WIDTH, LCD_HEIGHT,
        LCD_COL_OFFSET, LCD_ROW_OFFSET, LCD_COL_OFFSET, LCD_ROW_OFFSET);
}

void display_hal_begin(void) {
    gfx->begin();
    gfx->fillScreen(0x0000);
    ledcAttach(LCD_BL, 5000 /* Hz */, 8 /* bits */);
    ledcWrite(LCD_BL, 200);
}

void display_hal_set_brightness(uint8_t level) {
    ledcWrite(LCD_BL, level);
}

void display_hal_draw_bitmap(int32_t x, int32_t y, int32_t w, int32_t h,
                             const uint16_t* pixels) {
    if (gfx) gfx->draw16bitRGBBitmap(x, y, (uint16_t*)pixels, w, h);
}

void display_hal_tick(void) {
    // No rotation on this board.
}

// ST7789 has no flush-region alignment requirement.
void display_hal_round_area(int32_t* x1, int32_t* y1, int32_t* x2, int32_t* y2) {
    (void)x1; (void)y1; (void)x2; (void)y2;
}
