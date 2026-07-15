#include "../../hal/display_hal.h"
#include "board.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>

// SpotPear 1.54" MUMA — plain 4-wire SPI ST7789 (240x240 IPS). No QSPI, no
// AMOLED brightness command, no CPU rotation. Backlight is a GPIO driven by
// LEDC PWM and is ACTIVE-LOW (LCD_BL_INVERT), so brightness duty is inverted.

static Arduino_DataBus* bus = nullptr;
static Arduino_ST7789*  gfx = nullptr;

static const int      BL_PWM_FREQ = 5000;   // Hz
static const uint8_t  BL_PWM_BITS = 8;      // 0..255 duty

void display_hal_init(void) {
    // Arduino_ESP32SPI(dc, cs, sck, mosi, miso) — explicit SPI GPIOs.
    bus = new Arduino_ESP32SPI(LCD_DC, LCD_CS, LCD_SCK, LCD_MOSI, GFX_NOT_DEFINED);
    // Arduino_ST7789(bus, rst, rotation, ips, w, h, col1, row1, col2, row2).
    // ips=true sends INVON — matches the board's invert_color=true.
    gfx = new Arduino_ST7789(
        bus, LCD_RST, 0 /* rotation */, true /* IPS */,
        LCD_WIDTH, LCD_HEIGHT,
        LCD_COL_OFFSET, LCD_ROW_OFFSET, LCD_COL_OFFSET, LCD_ROW_OFFSET);
}

void display_hal_begin(void) {
    gfx->begin(LCD_SPI_HZ);
    gfx->fillScreen(0x0000);

    ledcAttach(LCD_BL, BL_PWM_FREQ, BL_PWM_BITS);
    display_hal_set_brightness(200);
}

void display_hal_set_brightness(uint8_t level) {
#if LCD_BL_INVERT
    ledcWrite(LCD_BL, 255 - level);   // active-low backlight
#else
    ledcWrite(LCD_BL, level);
#endif
}

void display_hal_fill_screen(uint16_t color) {
    if (gfx) gfx->fillScreen(color);
}

void display_hal_draw_bitmap(int32_t x, int32_t y, int32_t w, int32_t h,
                             const uint16_t* pixels) {
    // Fixed orientation, native panel — push straight through.
    if (gfx) gfx->draw16bitRGBBitmap(x, y, (uint16_t*)pixels, w, h);
}

void display_hal_tick(void) {
    // No software rotation on this board — nothing to do.
}

void display_hal_round_area(int32_t* x1, int32_t* y1, int32_t* x2, int32_t* y2) {
    // ST7789 has no even-alignment requirement — leave the area as-is.
    (void)x1; (void)y1; (void)x2; (void)y2;
}
