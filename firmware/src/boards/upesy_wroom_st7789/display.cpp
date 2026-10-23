#include "../../hal/display_hal.h"
#include "board.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>

// ST7789 over plain 4-wire SPI on the ESP32's native VSPI pins. The CYD's bus
// setup with the T-Display S3's panel geometry: Arduino_GFX applies the 35 px
// column offset per rotation, brightness is LEDC PWM on the backlight GPIO,
// and there is no flush-alignment requirement.
//
// Inversion is ON (`ips`): these IPS modules render a photo-negative image
// without it. Build with -DST7789_NO_INVERT if yours comes out inverted.

static Arduino_DataBus* bus = nullptr;
static Arduino_ST7789*  gfx = nullptr;

void display_hal_init(void) {
    // Write-only panel: no MISO claimed.
    bus = new Arduino_ESP32SPI(LCD_DC, LCD_CS, LCD_SCLK, LCD_MOSI,
                               GFX_NOT_DEFINED /* no MISO */, VSPI);

#ifdef ST7789_NO_INVERT
    const bool ips = false;
#else
    const bool ips = true;
#endif

    gfx = new Arduino_ST7789(
        bus, LCD_RST, LCD_ROTATION, ips,
        LCD_NATIVE_WIDTH, LCD_NATIVE_HEIGHT,
        LCD_COL_OFFSET, LCD_ROW_OFFSET, LCD_COL_OFFSET, LCD_ROW_OFFSET);
}

void display_hal_begin(void) {
    gfx->begin(LCD_SPI_SPEED);
    gfx->fillScreen(0x0000);

    // Backlight comes up only after the panel has been cleared — board_init()
    // parked it LOW so power-on GRAM noise never reaches the screen.
    ledcAttach(LCD_BL, 5000 /* Hz */, 8 /* bits */);
    ledcWrite(LCD_BL, 200);

    Serial.printf("Display ST7789 ready (%dx%d, VSPI %d MHz)\n",
                  LCD_WIDTH, LCD_HEIGHT, LCD_SPI_SPEED / 1000000);
}

void display_hal_set_brightness(uint8_t level) {
    ledcWrite(LCD_BL, level);
}

void display_hal_draw_bitmap(int32_t x, int32_t y, int32_t w, int32_t h,
                             const uint16_t* pixels) {
    if (gfx) gfx->draw16bitRGBBitmap(x, y, (uint16_t*)pixels, w, h);
}

void display_hal_tick(void) {
    // No rotation cycle on this board.
}

// ST7789 over SPI has no flush-region alignment requirement.
void display_hal_round_area(int32_t* x1, int32_t* y1, int32_t* x2, int32_t* y2) {
    (void)x1; (void)y1; (void)x2; (void)y2;
}
