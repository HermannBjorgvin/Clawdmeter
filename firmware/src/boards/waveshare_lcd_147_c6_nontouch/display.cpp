#include "../../hal/display_hal.h"
#include "board.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>

// Matches the Waveshare C6 LCD pin definition used by the LVGL 9 example
// linked from Waveshare's board documentation. The SD card shares this bus.
static Arduino_DataBus* bus;
static Arduino_ST7789* gfx;

void display_hal_init(void) {
    pinMode(SD_CS, OUTPUT); // SD chip select: deselect the card on the shared bus.
    digitalWrite(SD_CS, HIGH);
    bus = new Arduino_HWSPI(LCD_DC, LCD_CS, LCD_SCLK, LCD_MOSI, SPI_MISO);
    gfx = new Arduino_ST7789(bus, LCD_RST, LCD_ROTATION, true,
                             LCD_PANEL_W, LCD_PANEL_H,
                             LCD_COL_OFFSET, 0, LCD_COL_OFFSET, 0);
}

void display_hal_begin(void) {
    gfx->begin(40000000);
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
    if (gfx) gfx->draw16bitRGBBitmap(x, y, pixels, w, h);
}
void display_hal_tick(void) {}
void display_hal_round_area(int32_t* x1, int32_t* y1, int32_t* x2, int32_t* y2) {
    (void)x1; (void)y1; (void)x2; (void)y2;
}
