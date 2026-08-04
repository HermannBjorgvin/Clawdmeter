#include "../../hal/display_hal.h"
#include "board.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>

// ST7701S uses 3-wire SPI (CS/SCLK/MOSI) for the register init sequence and
// a 16-bit RGB parallel interface (DE/HSYNC/VSYNC/PCLK + R[4:0]/G[5:0]/B[4:0])
// for pixel data. The ESP32-S3 LCD DMA controller drives the RGB side via a
// full-screen framebuffer in PSRAM — it refreshes the panel continuously, so
// partial LVGL flush writes go straight into that framebuffer and are visible
// on the very next DMA cycle. No explicit flush management needed.
//
// setBrightness() is not available on RGB displays; backlight is PWM on GPIO 38.

static Arduino_DataBus*    spi_bus  = nullptr;
static Arduino_ESP32RGBPanel* panel = nullptr;
static Arduino_RGB_Display*   gfx   = nullptr;

// ST7701S init sequence for the Generic Guition 4848S040, sourced from the
// OpenHASP project (lib/Arduino_RPi_DPI_RGBPanel_mod/Arduino_RGB_Display_mod.h).
// Enum values (BEGIN_WRITE, WRITE_COMMAND_8, …) come from Arduino_DataBus.h.
static const uint8_t st7701_init_ops[] = {
    BEGIN_WRITE,
    WRITE_COMMAND_8, 0xFF,
    WRITE_BYTES, 5, 0x77, 0x01, 0x00, 0x00, 0x10,

    WRITE_C8_D16, 0xC0, 0x3B, 0x00,
    WRITE_C8_D16, 0xC1, 0x0D, 0x02,
    WRITE_C8_D16, 0xC2, 0x31, 0x05,
    WRITE_C8_D8,  0xCD, 0x00,

    WRITE_COMMAND_8, 0xB0,  // Positive Voltage Gamma
    WRITE_BYTES, 16,
    0x00, 0x11, 0x18, 0x0E,
    0x11, 0x06, 0x07, 0x08,
    0x07, 0x22, 0x04, 0x12,
    0x0F, 0xAA, 0x31, 0x18,

    WRITE_COMMAND_8, 0xB1,  // Negative Voltage Gamma
    WRITE_BYTES, 16,
    0x00, 0x11, 0x19, 0x0E,
    0x12, 0x07, 0x08, 0x08,
    0x08, 0x22, 0x04, 0x11,
    0x11, 0xA9, 0x32, 0x18,

    // PAGE1
    WRITE_COMMAND_8, 0xFF,
    WRITE_BYTES, 5, 0x77, 0x01, 0x00, 0x00, 0x11,

    WRITE_C8_D8, 0xB0, 0x60,   // Vop = 4.7375 V
    WRITE_C8_D8, 0xB1, 0x32,   // VCOM
    WRITE_C8_D8, 0xB2, 0x07,   // VGH = 15 V
    WRITE_C8_D8, 0xB3, 0x80,
    WRITE_C8_D8, 0xB5, 0x49,   // VGL = −10.17 V
    WRITE_C8_D8, 0xB7, 0x85,
    WRITE_C8_D8, 0xB8, 0x21,   // AVDD = 6.6 V, AVCL = −4.6 V
    WRITE_C8_D8, 0xC1, 0x78,
    WRITE_C8_D8, 0xC2, 0x78,

    WRITE_COMMAND_8, 0xE0,
    WRITE_BYTES, 3, 0x00, 0x1B, 0x02,

    WRITE_COMMAND_8, 0xE1,
    WRITE_BYTES, 11,
    0x08, 0xA0, 0x00, 0x00,
    0x07, 0xA0, 0x00, 0x00,
    0x00, 0x44, 0x44,

    WRITE_COMMAND_8, 0xE2,
    WRITE_BYTES, 12,
    0x11, 0x11, 0x44, 0x44,
    0xED, 0xA0, 0x00, 0x00,
    0xEC, 0xA0, 0x00, 0x00,

    WRITE_COMMAND_8, 0xE3,
    WRITE_BYTES, 4, 0x00, 0x00, 0x11, 0x11,

    WRITE_C8_D16, 0xE4, 0x44, 0x44,

    WRITE_COMMAND_8, 0xE5,
    WRITE_BYTES, 16,
    0x0A, 0xE9, 0xD8, 0xA0,
    0x0C, 0xEB, 0xD8, 0xA0,
    0x0E, 0xED, 0xD8, 0xA0,
    0x10, 0xEF, 0xD8, 0xA0,

    WRITE_COMMAND_8, 0xE6,
    WRITE_BYTES, 4, 0x00, 0x00, 0x11, 0x11,

    WRITE_C8_D16, 0xE7, 0x44, 0x44,

    WRITE_COMMAND_8, 0xE8,
    WRITE_BYTES, 16,
    0x09, 0xE8, 0xD8, 0xA0,
    0x0B, 0xEA, 0xD8, 0xA0,
    0x0D, 0xEC, 0xD8, 0xA0,
    0x0F, 0xEE, 0xD8, 0xA0,

    WRITE_COMMAND_8, 0xEB,
    WRITE_BYTES, 7,
    0x02, 0x00, 0xE4, 0xE4,
    0x88, 0x00, 0x40,

    WRITE_C8_D16, 0xEC, 0x3C, 0x00,

    WRITE_COMMAND_8, 0xED,
    WRITE_BYTES, 16,
    0xAB, 0x89, 0x76, 0x54,
    0x02, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0x20,
    0x45, 0x67, 0x98, 0xBA,

    // VAP / VAN bank
    WRITE_COMMAND_8, 0xFF,
    WRITE_BYTES, 5, 0x77, 0x01, 0x00, 0x00, 0x13,
    WRITE_C8_D8, 0xE5, 0xE4,

    WRITE_COMMAND_8, 0xFF,
    WRITE_BYTES, 5, 0x77, 0x01, 0x00, 0x00, 0x00,

    WRITE_COMMAND_8, 0x21,          // IPS mode
    WRITE_C8_D8, 0x3A, 0x60,        // RGB666 pixel format

    WRITE_COMMAND_8, 0x11,          // Sleep Out
    END_WRITE,

    DELAY, 120,

    BEGIN_WRITE,
    WRITE_COMMAND_8, 0x29,          // Display On
    END_WRITE,

    BEGIN_WRITE,
    WRITE_COMMAND_8, 0x20,          // Display Inversion Off (normal)
    END_WRITE,
};

void display_hal_init(void) {
    // 3-wire SPI bus: DC=-1 (not used for RGB init protocol), no MISO
    spi_bus = new Arduino_SWSPI(
        GFX_NOT_DEFINED /* DC */, TFT_CS, TFT_SCLK, TFT_MOSI, GFX_NOT_DEFINED /* MISO */);

    panel = new Arduino_ESP32RGBPanel(
        TFT_DE, TFT_VSYNC, TFT_HSYNC, TFT_PCLK,
        TFT_R0, TFT_R1, TFT_R2, TFT_R3, TFT_R4,
        TFT_G0, TFT_G1, TFT_G2, TFT_G3, TFT_G4, TFT_G5,
        TFT_B0, TFT_B1, TFT_B2, TFT_B3, TFT_B4,
        TFT_HSYNC_POLARITY, TFT_HSYNC_FRONT_PORCH, TFT_HSYNC_PULSE_WIDTH, TFT_HSYNC_BACK_PORCH,
        TFT_VSYNC_POLARITY, TFT_VSYNC_FRONT_PORCH, TFT_VSYNC_PULSE_WIDTH, TFT_VSYNC_BACK_PORCH,
        TFT_PCLK_ACTIVE_NEG, TFT_PREFER_SPEED);

    gfx = new Arduino_RGB_Display(
        LCD_WIDTH, LCD_HEIGHT, panel,
        0 /* rotation */, false /* auto_flush — LVGL drives flushing */,
        spi_bus, GFX_NOT_DEFINED /* RST */,
        st7701_init_ops, sizeof(st7701_init_ops));
}

void display_hal_begin(void) {
    gfx->begin(GFX_NOT_DEFINED);    // speed ignored for RGB panel
    gfx->fillScreen(0x0000);

    // Backlight: Arduino 3.x LEDC API (pioarduino / ESP-IDF 5.x)
    // NOTE: 100 Hz with 8-bit resolution overflows the 18-bit ESP32-S3 LEDC
    // clock_divider register (needs div=800,000; max is 262,143 at 80 MHz APB).
    // Minimum safe frequency at 8-bit/80 MHz is ~305 Hz; use 1 kHz.
    ledcAttach(TFT_BCKL, 1000, 8);  // 1 kHz, 8-bit resolution
    ledcWrite(TFT_BCKL, 200);        // default brightness (~78 %)
}

void display_hal_set_brightness(uint8_t level) {
    ledcWrite(TFT_BCKL, level);
}

void display_hal_fill_screen(uint16_t color) {
    if (gfx) gfx->fillScreen(color);
}

void display_hal_draw_bitmap(int32_t x, int32_t y, int32_t w, int32_t h,
                             const uint16_t* pixels) {
    if (!gfx) return;
    // Use memcpy row-by-row directly into the DMA framebuffer instead of the
    // GFX library's word-loop. gfx_draw_bitmap_to_framebuffer copies 2 pixels
    // per iteration (~1.9 ms for a 480x40 strip); memcpy uses the compiler's
    // optimised path (~0.5 ms), halving the window during which the DMA and
    // CPU race over the same PSRAM region and reducing visible tearing.
    uint16_t* fb = gfx->getFramebuffer();
    if (!fb) return;
    for (int32_t row = 0; row < h; row++) {
        memcpy(&fb[(y + row) * LCD_WIDTH + x],
               &pixels[row * w],
               (size_t)w * sizeof(uint16_t));
    }
}

void display_hal_tick(void) {
    // No rotation, no transition handling needed.
}

void display_hal_round_area(int32_t* x1, int32_t* y1, int32_t* x2, int32_t* y2) {
    // RGB parallel panel has no alignment constraint; this is a no-op.
    (void)x1; (void)y1; (void)x2; (void)y2;
}
