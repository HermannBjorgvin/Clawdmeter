#include "../../hal/display_hal.h"
#include "board.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>

// ILI9342C over 4-wire SPI (VSPI) — the M5Stack Core/Fire panel. Arduino_GFX
// ships a dedicated Arduino_ILI9342 driver whose rotation table already applies
// the panel's BGR order, so the HAL surface is identical to the ST7789 port.
// Brightness is a LEDC PWM duty on the backlight GPIO (the TFT has no in-panel
// brightness command), same approach as the LCD-1.54.
//
// Rotation 2 gives the native 320x240 landscape upright with the three front
// buttons along the bottom edge (rotation 0 renders it 180°-flipped — upside
// down — on this hardware; both are 320x240, just mirrored 180°).

static Arduino_DataBus*  bus = nullptr;
static Arduino_ILI9342*  gfx = nullptr;

void display_hal_init(void) {
    bus = new Arduino_ESP32SPI(LCD_DC, LCD_CS, LCD_SCLK, LCD_MOSI, LCD_MISO);
    // ips=true — the M5Stack panel needs color inversion for correct blacks.
    gfx = new Arduino_ILI9342(bus, LCD_RST, 2 /* rotation */, true /* ips */);
}

void display_hal_begin(void) {
    gfx->begin(40000000);   // 40 MHz — the M5Stack-library default for this panel
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

// ILI9342C over SPI has no flush-region alignment requirement.
void display_hal_round_area(int32_t* x1, int32_t* y1, int32_t* x2, int32_t* y2) {
    (void)x1; (void)y1; (void)x2; (void)y2;
}
