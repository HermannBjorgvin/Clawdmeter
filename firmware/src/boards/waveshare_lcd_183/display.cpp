#include "../../hal/display_hal.h"
#include "board.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>

// Waveshare LCD-1.83: ST7789P IPS panel over 4-wire SPI, fixed 0° (no rotation,
// no CPU pixel remap). Unlike the QSPI AMOLED ports, the panel is backlit by a
// GPIO LED string, so display_hal_set_brightness() drives an LEDC PWM channel
// on LCD_BL instead of issuing an AMOLED brightness command. Geometry/offsets
// match Waveshare's demo: Arduino_ST7789(bus, RST, 0, IPS, 240, 284, 0, 20).

#define BL_PWM_FREQ   5000
#define BL_PWM_BITS   8

static Arduino_DataBus* bus = nullptr;
static Arduino_GFX*     gfx = nullptr;

void display_hal_init(void) {
    bus = new Arduino_ESP32SPI(LCD_DC, LCD_CS, LCD_SCK, LCD_MOSI);
    // (bus, rst, rotation, IPS, w, h, col_off1, row_off1, col_off2, row_off2)
    gfx = new Arduino_ST7789(
        bus, LCD_RST, 0 /*rotation*/, true /*IPS*/,
        LCD_WIDTH, LCD_HEIGHT, 0, LCD_ROW_OFFSET, 0, 0);
}

void display_hal_begin(void) {
    gfx->begin();
    gfx->fillScreen(0x0000);
    // Enable the backlight only after the panel is initialized + cleared, so
    // there's no white flash on boot. LEDC gives us real brightness control.
    ledcAttach(LCD_BL, BL_PWM_FREQ, BL_PWM_BITS);
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
    // No rotation handling on this board.
}

void display_hal_round_area(int32_t* x1, int32_t* y1, int32_t* x2, int32_t* y2) {
    // ST7789 has no even-alignment constraint on flush regions.
    (void)x1; (void)y1; (void)x2; (void)y2;
}
