#include "../../hal/display_hal.h"
#include "board.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>

static Arduino_DataBus*       spi_init_bus = nullptr;
static Arduino_ESP32RGBPanel* rgb_panel    = nullptr;
static Arduino_RGB_Display*   gfx         = nullptr;

void display_hal_init(void) {}
void display_hal_begin(void) {}
void display_hal_set_brightness(uint8_t level) { (void)level; }
void display_hal_fill_screen(uint16_t color) { (void)color; }
void display_hal_draw_bitmap(int32_t x, int32_t y, int32_t w, int32_t h,
                             const uint16_t* pixels) {
    (void)x; (void)y; (void)w; (void)h; (void)pixels;
}
void display_hal_tick(void) {}
void display_hal_round_area(int32_t* x1, int32_t* y1, int32_t* x2, int32_t* y2) {
    (void)x1; (void)y1; (void)x2; (void)y2;
}
