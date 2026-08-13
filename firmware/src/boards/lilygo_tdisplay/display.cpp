#include "../../hal/display_hal.h"
#include "board.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>

// ST7789 over plain 4-wire SPI — the same driver as esp32_devkit_st7789, with
// this board's window offsets (see board.h) instead of that port's row offset.
//
// 40 MHz: the panel is soldered to the PCB with short traces, so it takes the
// same clock the DIY jumper-wired port runs at with room to spare.
//
// ips=true inverts colors, which is what this IPS module wants. If the image
// comes out photo-negative, that flag is the whole fix.

#define LCD_SPI_HZ  40000000

static Arduino_DataBus* bus = nullptr;
static Arduino_ST7789*  gfx = nullptr;

void display_hal_init(void) {
    bus = new Arduino_ESP32SPI(LCD_DC, LCD_CS, LCD_SCLK, LCD_MOSI,
                               GFX_NOT_DEFINED /* no MISO */);
    // Panel-native size + rotation: the controller does the row/column exchange
    // itself, so LVGL is handed a 240x135 landscape surface with no per-frame
    // cost (see board.h). The four offsets go in unchanged — the library picks
    // the right pair for the rotation.
    gfx = new Arduino_ST7789(bus, LCD_RST, LCD_ROTATION, true /* ips */,
                             LCD_PANEL_W, LCD_PANEL_H,
                             LCD_COL_OFFSET1, LCD_ROW_OFFSET1,
                             LCD_COL_OFFSET2, LCD_ROW_OFFSET2);
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
