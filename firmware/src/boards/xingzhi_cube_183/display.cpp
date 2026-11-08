#include "../../hal/display_hal.h"
#include "board.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>

// Vendor init table for this panel (from the board's ESPHome config). It
// replaces Arduino_ST7789's default init operations: the library sends the
// hardware reset and SWRESET first, then runs these. Ends with sleep-out,
// 16-bit colour, normal display on and display on. Inversion is applied by
// the driver afterwards (ips = true in the constructor below).
static const uint8_t xingzhi_init_ops[] = {
    BEGIN_WRITE,
    WRITE_C8_BYTES, 0xFD, 2, 0x06, 0x08,
    WRITE_C8_BYTES, 0x61, 2, 0x07, 0x04,
    WRITE_C8_BYTES, 0x62, 3, 0x00, 0x44, 0x45,
    WRITE_C8_BYTES, 0x63, 4, 0x41, 0x07, 0x12, 0x12,
    WRITE_C8_D8,    0x64, 0x37,
    WRITE_C8_BYTES, 0x65, 3, 0x09, 0x10, 0x21,
    WRITE_C8_BYTES, 0x66, 3, 0x09, 0x10, 0x21,
    WRITE_C8_BYTES, 0x67, 2, 0x20, 0x40,
    WRITE_C8_BYTES, 0x68, 4, 0x90, 0x4C, 0x7C, 0x66,
    WRITE_C8_BYTES, 0xB1, 3, 0x0F, 0x02, 0x01,
    WRITE_C8_D8,    0xB4, 0x01,
    WRITE_C8_BYTES, 0xB5, 4, 0x02, 0x02, 0x0A, 0x14,
    WRITE_C8_BYTES, 0xB6, 5, 0x04, 0x01, 0x9F, 0x00, 0x02,
    WRITE_C8_D8,    0xDF, 0x11,
    WRITE_C8_BYTES, 0xE2, 6, 0x13, 0x00, 0x00, 0x30, 0x33, 0x3F,
    WRITE_C8_BYTES, 0xE5, 6, 0x3F, 0x33, 0x30, 0x00, 0x00, 0x13,
    WRITE_C8_BYTES, 0xE1, 2, 0x00, 0x57,
    WRITE_C8_BYTES, 0xE4, 2, 0x58, 0x00,
    WRITE_C8_BYTES, 0xE0, 8, 0x01, 0x03, 0x0D, 0x0E, 0x0E, 0x0C, 0x15, 0x19,
    WRITE_C8_BYTES, 0xE3, 8, 0x1A, 0x16, 0x0C, 0x0F, 0x0E, 0x0D, 0x02, 0x01,
    WRITE_C8_BYTES, 0xE6, 2, 0x00, 0xFF,
    WRITE_C8_BYTES, 0xE7, 6, 0x01, 0x04, 0x03, 0x03, 0x00, 0x12,
    WRITE_C8_BYTES, 0xE8, 3, 0x00, 0x70, 0x00,
    WRITE_C8_D8,    0xEC, 0x52,
    WRITE_C8_BYTES, 0xF1, 3, 0x01, 0x01, 0x02,
    WRITE_C8_BYTES, 0xF6, 4, 0x09, 0x10, 0x00, 0x00,
    WRITE_C8_BYTES, 0xFD, 2, 0xFA, 0xFC,
    WRITE_C8_D8,    0x35, 0x00,        // TEON
    WRITE_COMMAND_8, 0x11,             // SLPOUT
    END_WRITE,

    DELAY, 120,

    BEGIN_WRITE,
    WRITE_C8_D8,    0x3A, 0x55,        // COLMOD: RGB565
    WRITE_COMMAND_8, 0x13,             // NORON
    WRITE_COMMAND_8, 0x29,             // DISPON
    END_WRITE,
};

static Arduino_DataBus* bus = nullptr;
static Arduino_ST7789*  gfx = nullptr;

void display_hal_init(void) {
    bus = new Arduino_ESP32SPI(LCD_DC, LCD_CS, LCD_SPI_CLK, LCD_SPI_MOSI);
    // Native 240x284 portrait window, rotation 3 (MADCTL MY|MV) = 284x240
    // landscape. For rotation 3 the library takes the x start from
    // row_offset2, so the panel's 36-column offset goes there.
    // ips = true: this panel is used with colour inversion on.
    gfx = new Arduino_ST7789(bus, LCD_RST, 3 /* rotation */, true /* ips */,
                             240, 284,
                             0 /* col_offset1 */, 0 /* row_offset1 */,
                             0 /* col_offset2 */, 36 /* row_offset2 */,
                             xingzhi_init_ops, sizeof(xingzhi_init_ops));
}

void display_hal_begin(void) {
    gfx->begin();
    // The board's ESPHome config sets colour order BGR; Arduino_ST7789 only
    // writes the RGB MADCTL variant, so set MADCTL here: MY | MV | BGR.
    bus->beginWrite();
    bus->writeC8D8(0x36, 0xA8);
    bus->endWrite();
    gfx->fillScreen(0x0000);
    ledcAttach(LCD_BL, 5000 /* Hz */, 8 /* bits */);
    ledcWrite(LCD_BL, 200);
}

void display_hal_set_brightness(uint8_t level) {
    ledcWrite(LCD_BL, level);
}

void display_hal_draw_bitmap(int32_t x, int32_t y, int32_t w, int32_t h,
                             const uint16_t* pixels) {
    if (gfx) gfx->draw16bitRGBBitmap(x, y, (uint16_t*)pixels, w, h);
}

void display_hal_tick(void) {
    // No rotation on this board.
}

// SPI TFT: no flush-region alignment requirement.
void display_hal_round_area(int32_t* x1, int32_t* y1, int32_t* x2, int32_t* y2) {
    (void)x1; (void)y1; (void)x2; (void)y2;
}
