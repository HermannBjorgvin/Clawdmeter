#include "../../hal/display_hal.h"
#include "board.h"
#include "pmu.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include "../common/ili9342_m5.inc"

// ILI9342C or, on later units, ILI9342E (probed at boot), on the Core2's pins,
// with M5GFX's init lists (ili9342_m5.inc). The reset line was pulsed by
// pmu_init() in board_init(), so the driver gets no RST pin; brightness is the
// power chip's backlight rail.

static Arduino_DataBus* bus = nullptr;
static M5_ILI9342* gfx = nullptr;
static Ili9342Variant panel_variant = ILI9342_VARIANT_UNKNOWN;

const char* core2_panel_name(void) { return ili9342_variant_name(panel_variant); }

void display_hal_init(void) {
    panel_variant = ili9342_probe(150);
    bus = new Arduino_ESP32SPI(LCD_DC, LCD_CS, LCD_SCLK, LCD_MOSI, LCD_MISO);
    gfx = new M5_ILI9342(bus, GFX_NOT_DEFINED, 2 /* rotation */, true /* ips */, panel_variant);
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
