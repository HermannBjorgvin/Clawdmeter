#include "../../hal/display_hal.h"
#include "board.h"
#include "io_expander.h"
#include "power_state.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <esp_heap_caps.h>

// ST7701 over ESP32 RGB parallel. Arduino_GFX DMA-scans the PSRAM frame
// buffer; bounce_buffer_size_px gives ESP-IDF two SRAM bounce buffers so
// CPU writes to PSRAM don't tear against the DMA scan.
//
// Do not call rgbpanel->getFrameBuffer() after gfx->begin() — that
// reconstructs the RGB panel a second time and crashes (no free slot).

static Arduino_DataBus*      spi      = nullptr;
static Arduino_ESP32RGBPanel* rgbpanel = nullptr;
static Arduino_RGB_Display*  gfx      = nullptr;

static_assert(LCD_WIDTH == LCD_HEIGHT || LCD_ROTATION % 2 == 0,
              "90/270 rotation assumes a square panel");

// Scratch buffer for the rotated strip (sized to the largest LVGL flush seen).
// Arduino_RGB_Display's own setRotation path falls back to per-pixel writes,
// so we remap the whole strip here and blit it at native orientation.
static uint16_t* rot_buf     = nullptr;
static size_t    rot_buf_px  = 0;

static uint16_t* rot_scratch(size_t px) {
    if (px <= rot_buf_px) return rot_buf;
    heap_caps_free(rot_buf);
    rot_buf = (uint16_t*)heap_caps_malloc(px * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!rot_buf) rot_buf = (uint16_t*)heap_caps_malloc(px * 2, MALLOC_CAP_SPIRAM);
    rot_buf_px = rot_buf ? px : 0;
    return rot_buf;
}

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

// ST7701 has no panel brightness command; the backlight is the expander's
// PWM (CH32 boards) or an on/off pin (TCA boards). On battery the requested
// level is scaled down to save power.
#define ON_BATTERY_BRIGHTNESS_PCT 35   // 60 was too subtle to notice

static uint8_t requested_level = 255;

void display_hal_set_brightness(uint8_t level) {
    requested_level = level;
    uint8_t out = level;
    if (lcd4_on_battery()) out = (uint8_t)((level * ON_BATTERY_BRIGHTNESS_PCT + 50) / 100);
    if (level > 0 && out == 0) out = 1;   // a dimmed non-zero level stays on
    io_expander_set_brightness(out);
}

void lcd4_reapply_brightness(void) {
    display_hal_set_brightness(requested_level);
}

// Logical (LVGL) pixel (x, y) lands on panel pixel:
//   1: (W-1-y, x)   2: (W-1-x, H-1-y)   3: (y, H-1-x)
void display_hal_draw_bitmap(int32_t x, int32_t y, int32_t w, int32_t h,
                             const uint16_t* pixels) {
    if (!gfx) return;
    if (LCD_ROTATION == 0) {
        gfx->draw16bitRGBBitmap(x, y, (uint16_t*)pixels, w, h);
        return;
    }
    uint16_t* dst = rot_scratch((size_t)w * h);
    if (!dst) return;
    const int32_t n = w * h;
    switch (LCD_ROTATION) {
    case 1:   // dest is h wide, w tall
        for (int32_t j = 0; j < h; j++)
            for (int32_t i = 0; i < w; i++)
                dst[i * h + (h - 1 - j)] = pixels[j * w + i];
        gfx->draw16bitRGBBitmap(LCD_WIDTH - y - h, x, dst, h, w);
        break;
    case 2:   // 180°: same rect mirrored, pixel order reversed
        for (int32_t k = 0; k < n; k++) dst[n - 1 - k] = pixels[k];
        gfx->draw16bitRGBBitmap(LCD_WIDTH - x - w, LCD_HEIGHT - y - h, dst, w, h);
        break;
    case 3:
        for (int32_t j = 0; j < h; j++)
            for (int32_t i = 0; i < w; i++)
                dst[(w - 1 - i) * h + j] = pixels[j * w + i];
        gfx->draw16bitRGBBitmap(y, LCD_HEIGHT - x - w, dst, h, w);
        break;
    }
}

void display_hal_tick(void) {
    // No rotation cycle on this board.
}

void display_hal_round_area(int32_t* x1, int32_t* y1, int32_t* x2, int32_t* y2) {
    (void)x1; (void)y1; (void)x2; (void)y2;
}
