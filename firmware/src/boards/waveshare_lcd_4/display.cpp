#include "../../hal/display_hal.h"
#include "board.h"
#include "io_expander.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>

// ST7701 over ESP32 RGB parallel. Arduino_GFX DMA-scans the PSRAM frame
// buffer; bounce_buffer_size_px gives ESP-IDF two SRAM bounce buffers so
// CPU writes to PSRAM don't tear against the DMA scan.
//
// Do not call rgbpanel->getFrameBuffer() after gfx->begin() — that
// reconstructs the RGB panel a second time and crashes (no free slot).

static Arduino_DataBus*      spi      = nullptr;
static Arduino_ESP32RGBPanel* rgbpanel = nullptr;
static Arduino_RGB_Display*  gfx      = nullptr;

void display_hal_init(void) {
    spi = new Arduino_SWSPI(
        GFX_NOT_DEFINED /* DC */, LCD_SPI_CS,
        LCD_SPI_SCK, LCD_SPI_MOSI, GFX_NOT_DEFINED /* MISO */);
    rgbpanel = new Arduino_ESP32RGBPanel(
        LCD_DE, LCD_VSYNC, LCD_HSYNC, LCD_PCLK,
        LCD_R0, LCD_R1, LCD_R2, LCD_R3, LCD_R4,
        LCD_G0, LCD_G1, LCD_G2, LCD_G3, LCD_G4, LCD_G5,
        LCD_B0, LCD_B1, LCD_B2, LCD_B3, LCD_B4,
        1 /* hsync_polarity */, 10 /* hsync_front_porch */,
        8 /* hsync_pulse_width */, 50 /* hsync_back_porch */,
        1 /* vsync_polarity */, 10 /* vsync_front_porch */,
        8 /* vsync_pulse_width */, 20 /* vsync_back_porch */,
        0 /* pclk_active_neg */, GFX_NOT_DEFINED /* prefer_speed */,
        false /* useBigEndian */,
        0 /* de_idle_high */, 0 /* pclk_idle_high */,
        LCD_WIDTH * 10 /* bounce_buffer_size_px */);
    gfx = new Arduino_RGB_Display(
        LCD_WIDTH, LCD_HEIGHT, rgbpanel, 0 /* rotation */,
        true /* auto_flush */,
        spi, GFX_NOT_DEFINED /* RST */,
        st7701_type1_init_operations, sizeof(st7701_type1_init_operations));
}

void display_hal_begin(void) {
    if (!gfx) return;
    gfx->begin();
    gfx->fillScreen(0x0000);
    io_expander_set_backlight(true);
}

void display_hal_set_brightness(uint8_t level) {
    // ST7701 has no panel brightness command. Backlight is an expander GPIO
    // (on/off only) — idle fade therefore snaps off at 0 instead of dimming.
    io_expander_set_backlight(level > 0);
}

void display_hal_fill_screen(uint16_t color) {
    if (gfx) gfx->fillScreen(color);
}

void display_hal_draw_bitmap(int32_t x, int32_t y, int32_t w, int32_t h,
                             const uint16_t* pixels) {
    if (gfx) gfx->draw16bitRGBBitmap(x, y, (uint16_t*)pixels, w, h);
}

void display_hal_tick(void) {
    // No rotation cycle on this board.
}

void display_hal_round_area(int32_t* x1, int32_t* y1, int32_t* x2, int32_t* y2) {
    (void)x1; (void)y1; (void)x2; (void)y2;
}
