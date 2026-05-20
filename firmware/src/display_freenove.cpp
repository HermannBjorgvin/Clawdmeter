#include "display_freenove.h"
#include "freenove_board.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>

static Arduino_DataBus* bus = nullptr;
static Arduino_GFX* gfx = nullptr;

void display_init(void) {
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);

    bus = new Arduino_ESP32SPI(TFT_DC, TFT_CS, TFT_SCLK, TFT_MOSI, TFT_MISO);
    gfx = new Arduino_ILI9341(bus, TFT_RST, 0, true, LCD_WIDTH, LCD_HEIGHT);
    gfx->begin();
    gfx->fillScreen(0x0000);
}

void display_flush(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    if (!gfx) {
        lv_display_flush_ready(disp);
        return;
    }

    const int32_t w = area->x2 - area->x1 + 1;
    const int32_t h = area->y2 - area->y1 + 1;
    gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t*)px_map, w, h);
    lv_display_flush_ready(disp);
}
