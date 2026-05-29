#include "../../hal/display_hal.h"
#include "board.h"
#include "io_expander.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>

// ST7701S init sequence for SenseCap Indicator D1L.
// Based on Arduino_GFX type9 with two corrections:
//   1. Register 0xCD: 0x00 → 0x08  (wrong value produces a green tint on all content)
//   2. Register 0xC0 omitted entirely (LovyanGFX leaves it at default; no regressions)
static const uint8_t st7701_init_ops[] = {
    BEGIN_WRITE,
    WRITE_COMMAND_8, 0xFF,
    WRITE_BYTES, 5, 0x77, 0x01, 0x00, 0x00, 0x10,

    WRITE_C8_D16, 0xC1, 0x0D, 0x02,
    WRITE_C8_D16, 0xC2, 0x31, 0x05,
    WRITE_C8_D8,  0xCD, 0x08,

    WRITE_COMMAND_8, 0xB0,
    WRITE_BYTES, 16,
    0x00, 0x11, 0x18, 0x0E,
    0x11, 0x06, 0x07, 0x08,
    0x07, 0x22, 0x04, 0x12,
    0x0F, 0xAA, 0x31, 0x18,

    WRITE_COMMAND_8, 0xB1,
    WRITE_BYTES, 16,
    0x00, 0x11, 0x19, 0x0E,
    0x12, 0x07, 0x08, 0x08,
    0x08, 0x22, 0x04, 0x11,
    0x11, 0xA9, 0x32, 0x18,

    WRITE_COMMAND_8, 0xFF,
    WRITE_BYTES, 5, 0x77, 0x01, 0x00, 0x00, 0x11,

    WRITE_C8_D8, 0xB0, 0x60,
    WRITE_C8_D8, 0xB1, 0x32,
    WRITE_C8_D8, 0xB2, 0x07,
    WRITE_C8_D8, 0xB3, 0x80,
    WRITE_C8_D8, 0xB5, 0x49,
    WRITE_C8_D8, 0xB7, 0x85,
    WRITE_C8_D8, 0xB8, 0x21,
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

    WRITE_COMMAND_8, 0xFF,
    WRITE_BYTES, 5, 0x77, 0x01, 0x00, 0x00, 0x13,

    WRITE_C8_D8, 0xE5, 0xE4,

    WRITE_COMMAND_8, 0xFF,
    WRITE_BYTES, 5, 0x77, 0x01, 0x00, 0x00, 0x00,

    WRITE_C8_D8, 0x3A, 0x60,   // COLMOD=0x60 (RGB666, matches RP2040 boot config)

    WRITE_COMMAND_8, 0x11,     // Sleep Out
    END_WRITE,

    DELAY, 120,

    BEGIN_WRITE,
    WRITE_COMMAND_8, 0x21,     // Display Inversion ON (IPS panel)
    WRITE_COMMAND_8, 0x29,     // Display On
    END_WRITE,
};

static Arduino_DataBus*       spi_init_bus = nullptr;
static Arduino_ESP32RGBPanel* rgb_panel    = nullptr;
static Arduino_RGB_Display*   gfx         = nullptr;

void display_hal_init(void) {
    spi_init_bus = new Arduino_ESP32SPI(
        GFX_NOT_DEFINED, GFX_NOT_DEFINED,
        SENSECAP_LCD_SPI_SCK, SENSECAP_LCD_SPI_MOSI, GFX_NOT_DEFINED);

    rgb_panel = new Arduino_ESP32RGBPanel(
        SENSECAP_LCD_DE, SENSECAP_LCD_VSYNC, SENSECAP_LCD_HSYNC, SENSECAP_LCD_PCLK,
        SENSECAP_LCD_R0, SENSECAP_LCD_R1, SENSECAP_LCD_R2, SENSECAP_LCD_R3, SENSECAP_LCD_R4,
        SENSECAP_LCD_G0, SENSECAP_LCD_G1, SENSECAP_LCD_G2, SENSECAP_LCD_G3,
        SENSECAP_LCD_G4, SENSECAP_LCD_G5,
        SENSECAP_LCD_B0, SENSECAP_LCD_B1, SENSECAP_LCD_B2, SENSECAP_LCD_B3, SENSECAP_LCD_B4,
        1, 10, 8, 50,   // hsync: polarity=1, fp=10, pw=8, bp=50
        1, 10, 8, 20,   // vsync: polarity=1, fp=10, pw=8, bp=20
        0, 12000000,    // pclk_active_neg=0, prefer_speed=12 MHz
        false, 0, 0,    // useBigEndian, de_idle_high, pclk_idle_high
        480 * 10);      // bounce_buffer_size_px — SRAM relay cuts PSRAM bus contention

    gfx = new Arduino_RGB_Display(
        LCD_WIDTH, LCD_HEIGHT, rgb_panel,
        2,    // rotation 2 = 180°; panel is mounted upside-down on this board
        true, // auto_flush
        spi_init_bus, GFX_NOT_DEFINED,
        st7701_init_ops, sizeof(st7701_init_ops));
}

void display_hal_begin(void) {
    // Assert LCD SPI CS (P04 LOW) so the ST7701S receives the init sequence.
    // The PCA9535 output register defaults to HIGH at power-on, so without this
    // step every SPI command is ignored and the display keeps the RP2040's
    // boot-time settings (wrong colours).
    io_expander_set(SENSECAP_LCD_CS_PORT, false);
    gfx->begin();
    io_expander_set(SENSECAP_LCD_CS_PORT, true);

    gfx->fillScreen(0x0000);

    // PWM backlight — 8-bit resolution at 1 kHz.
    ledcAttach(SENSECAP_BACKLIGHT, 1000, 8);
    ledcWrite(SENSECAP_BACKLIGHT, 200);   // matches DISPLAY_DEFAULT_BRIGHTNESS
}

void display_hal_set_brightness(uint8_t level) {
    ledcWrite(SENSECAP_BACKLIGHT, level);
}

void display_hal_fill_screen(uint16_t color) {
    if (gfx) gfx->fillScreen(color);
}

void display_hal_draw_bitmap(int32_t x, int32_t y, int32_t w, int32_t h,
                             const uint16_t* pixels) {
    if (gfx) gfx->draw16bitRGBBitmap(x, y, (uint16_t*)pixels, w, h);
}

void display_hal_tick(void) {}   // no rotation, no brightness ramp needed

void display_hal_round_area(int32_t* x1, int32_t* y1, int32_t* x2, int32_t* y2) {
    // RGB parallel panel has no even-alignment requirement; no-op.
    (void)x1; (void)y1; (void)x2; (void)y2;
}
