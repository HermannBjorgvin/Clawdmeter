#include "../../hal/display_hal.h"
#include "board.h"
#include "st77916_init.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>

// ST77916 is a true TFT (Arduino_TFT subclass), not an OLED — it has no
// command-based brightness register, so unlike the AMOLED boards' OLED
// drivers (which implement setBrightness() over QSPI), backlight dimming
// here is done with a separate LEDC PWM channel on LCD_BACKLIGHT_PIN.
//
// Arduino_GFX's built-in default init table (st77916_180_init_operations) is
// tuned for a different physical panel batch and produces a garbled image on
// this board — see st77916_init.h for Waveshare's own vendor-tuned "version_2"
// sequence (verified against this hardware; "version_1" is the other panel
// batch and renders solid black on this unit), plus an explicit COLMOD write
// Waveshare's table omits — without it colors render as uniform gray.

static Arduino_DataBus*   bus = nullptr;
static Arduino_ST77916*   gfx = nullptr;

void display_hal_init(void) {
    bus = new Arduino_ESP32QSPI(
        LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
    gfx = new Arduino_ST77916(bus, LCD_RESET, 0, true, LCD_WIDTH, LCD_HEIGHT,
                              0, 0, 0, 0,
                              st77916_waveshare_185b_init_operations_v2,
                              sizeof(st77916_waveshare_185b_init_operations_v2));

    ledcAttach(LCD_BACKLIGHT_PIN, 20000, 8);
}

void display_hal_begin(void) {
    gfx->begin(20000000);
    gfx->fillScreen(0x0000);
    display_hal_set_brightness(200);
}

void display_hal_set_brightness(uint8_t level) {
    ledcWrite(LCD_BACKLIGHT_PIN, LCD_BACKLIGHT_ON_LEVEL ? level : (255 - level));
}

void display_hal_fill_screen(uint16_t color) {
    if (gfx) gfx->fillScreen(color);
}

void display_hal_draw_bitmap(int32_t x, int32_t y, int32_t w, int32_t h,
                             const uint16_t* pixels) {
    if (gfx) gfx->draw16bitRGBBitmap(x, y, (uint16_t*)pixels, w, h);
}

void display_hal_tick(void) {
    // No rotation on this board — a round panel makes CPU rotation moot.
}

void display_hal_round_area(int32_t* x1, int32_t* y1, int32_t* x2, int32_t* y2) {
    *x1 = *x1 & ~1;
    *y1 = *y1 & ~1;
    *x2 = *x2 | 1;
    *y2 = *y2 | 1;
}
