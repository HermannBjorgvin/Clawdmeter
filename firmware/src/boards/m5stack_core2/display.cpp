#include "../../hal/display_hal.h"
#include "board.h"
#include "pmu.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>

// Same ILI9342C as the FIRE (Arduino_ILI9342, IPS inversion on), on the Core2's
// pins. The reset line was pulsed by pmu_init() in board_init(), so the driver
// gets no RST pin; brightness is the power chip's backlight rail.

static Arduino_DataBus* bus = nullptr;
static Arduino_ILI9342* gfx = nullptr;

void display_hal_init(void) {
    bus = new Arduino_ESP32SPI(LCD_DC, LCD_CS, LCD_SCLK, LCD_MOSI, LCD_MISO);
    gfx = new Arduino_ILI9342(bus, GFX_NOT_DEFINED, 2 /* rotation */, true /* ips */);
}

void display_hal_begin(void) {
    gfx->begin(40000000);
    gfx->fillScreen(0x0000);
    pmu_set_backlight(200);
}

void display_hal_set_brightness(uint8_t level) {
    pmu_set_backlight(level);
}

void display_hal_draw_bitmap(int32_t x, int32_t y, int32_t w, int32_t h,
                             const uint16_t* pixels) {
    if (gfx) gfx->draw16bitRGBBitmap(x, y, (uint16_t*)pixels, w, h);
}

void display_hal_tick(void) {}

void display_hal_round_area(int32_t* x1, int32_t* y1, int32_t* x2, int32_t* y2) {
    (void)x1; (void)y1; (void)x2; (void)y2;
}
