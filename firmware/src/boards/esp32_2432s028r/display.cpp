#include "../../hal/display_hal.h"
#include "board.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>

// ILI9341 over plain 4-wire SPI on the ESP32's native HSPI pins. Same shape as
// the LCD-1.54 ST7789 port — Arduino_GFX hides the bus, the panel has no
// brightness command so brightness is LEDC PWM on the backlight GPIO, and
// there is no flush-alignment requirement.
//
// PANEL VARIANTS. Sunton has shipped this model with more than one panel and
// more than one gamma table, and the differences are all cosmetic-but-obvious
// on first boot. Two build flags cover what has been seen in the wild:
//
//   -DCYD_ILI9341_TYPE2    washed-out / crushed colours → alternate init
//                          (the equivalent of TFT_eSPI's ILI9341_2_DRIVER)
//   -DCYD_INVERT_COLORS    photo-negative image → inverted panel
//
// Neither is needed on the common original (micro-USB) revision.

static Arduino_DataBus*  bus = nullptr;
static Arduino_ILI9341*  gfx = nullptr;

void display_hal_init(void) {
    // MISO is deliberately not claimed: the panel is write-only here, and
    // GPIO 12 is the MTDI strapping pin that selects flash voltage at reset.
    bus = new Arduino_ESP32SPI(LCD_DC, LCD_CS, LCD_SCLK, LCD_MOSI,
                               GFX_NOT_DEFINED /* no MISO */, HSPI);

#ifdef CYD_INVERT_COLORS
    const bool ips = true;
#else
    const bool ips = false;
#endif

#ifdef CYD_ILI9341_TYPE2
    gfx = new Arduino_ILI9341(bus, LCD_RST, LCD_ROTATION, ips,
                              LCD_NATIVE_WIDTH, LCD_NATIVE_HEIGHT,
                              0, 0, 0, 0,
                              ili9341_type2_init_operations,
                              sizeof(ili9341_type2_init_operations));
#else
    gfx = new Arduino_ILI9341(bus, LCD_RST, LCD_ROTATION, ips,
                              LCD_NATIVE_WIDTH, LCD_NATIVE_HEIGHT);
#endif
}

void display_hal_begin(void) {
    gfx->begin(LCD_SPI_SPEED);
    gfx->fillScreen(0x0000);

    // Backlight comes up only after the panel has been cleared — board_init()
    // parked it LOW so power-on GRAM noise never reaches the screen.
    ledcAttach(LCD_BL, 5000 /* Hz */, 8 /* bits */);
    ledcWrite(LCD_BL, 200);

    Serial.printf("Display ILI9341 ready (%dx%d, HSPI %d MHz)\n",
                  LCD_WIDTH, LCD_HEIGHT, LCD_SPI_SPEED / 1000000);
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
    // No rotation cycle on this board.
}

// ILI9341 over SPI has no flush-region alignment requirement.
void display_hal_round_area(int32_t* x1, int32_t* y1, int32_t* x2, int32_t* y2) {
    (void)x1; (void)y1; (void)x2; (void)y2;
}
