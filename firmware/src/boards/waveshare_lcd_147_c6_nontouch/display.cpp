#include "../../hal/display_hal.h"
#include "board.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>

static Arduino_DataBus* bus;
static Arduino_ST7789* gfx;

void display_hal_init(void) {
    bus = new Arduino_ESP32SPI(LCD_DC, LCD_CS, LCD_SCLK, LCD_MOSI, GFX_NOT_DEFINED);
    gfx = new Arduino_ST7789(bus, LCD_RST, 0, true, LCD_PANEL_W, LCD_PANEL_H,
                             LCD_COL_OFFSET, 0, LCD_COL_OFFSET, 0);
}

void display_hal_begin(void) {
    gfx->begin(40000000);
    // Panel-specific power and gamma values from Waveshare's ST7789 example.
    static const uint8_t panel_settings[] = {
        BEGIN_WRITE,
        WRITE_C8_D16, 0xB0, 0x00, 0xE8,
        WRITE_C8_D8, 0xBB, 0x35,
        WRITE_C8_D8, 0xC3, 0x13,
        WRITE_C8_D8, 0xD6, 0xA1,
        WRITE_C8_BYTES, 0xE0, 14,
        0xF0, 0x00, 0x04, 0x04, 0x04, 0x05, 0x29, 0x33,
        0x3E, 0x38, 0x12, 0x12, 0x28, 0x30,
        WRITE_C8_BYTES, 0xE1, 14,
        0xF0, 0x07, 0x0A, 0x0D, 0x0B, 0x07, 0x28, 0x33,
        0x3E, 0x36, 0x14, 0x14, 0x29, 0x32,
        END_WRITE,
    };
    bus->batchOperation(panel_settings, sizeof(panel_settings));
    gfx->setRotation(LCD_ROTATION);
    gfx->fillScreen(0);
    ledcAttach(LCD_BL, 5000, 8);
    display_hal_set_brightness(200);
}

void display_hal_set_brightness(uint8_t level) {
    // Waveshare advises at most 50% backlight to avoid overheating.
    ledcWrite(LCD_BL, ((uint32_t)level * 127) / 255);
}

void display_hal_draw_bitmap(int32_t x, int32_t y, int32_t w, int32_t h,
                             const uint16_t* pixels) {
    if (gfx) gfx->draw16bitRGBBitmap(x, y, (uint16_t*)pixels, w, h);
}
void display_hal_tick(void) {}
void display_hal_round_area(int32_t* x1, int32_t* y1, int32_t* x2, int32_t* y2) {
    (void)x1; (void)y1; (void)x2; (void)y2;
}
