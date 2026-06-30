#include "../../hal/display_hal.h"
#include "board.h"
#include "board_rev.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>

// 466x466 round panel. Reset is a direct GPIO (LCD_RESET), so the GFX driver
// drives it; unlike the 1.8 port we do NOT pass GFX_NOT_DEFINED.
//
// Display driver is INDEPENDENT of the touch controller on the 1.43: the panel
// is a CO5300 (verified on hardware + Waveshare spec) even on units fitted with
// an FT3168 (0x38) touch. The seeded code inferred the display from the touch
// address, which is wrong here — sending SH8601 init to a CO5300 leaves the
// panel black. The QSPI bus is write-only so we can't read display ID 0xDA to
// auto-detect; we drive CO5300 unconditionally. Flip DISPLAY_IS_SH8601 to 1
// only if you have a genuine early SH8601 panel.
#define DISPLAY_IS_SH8601  0
//
// ORIENTATION: the panel mounts 180° rotated, so the image comes up
// upside-down. We deliberately do NOT correct this in software — the device is
// physically rotated 180° in use, which fixes both the image and the touch
// mapping at once (touch.cpp returns raw coords to match). If you mount it the
// other way and want a software fix, the CO5300 driver's setRotation can't do
// it (its MADCTL flip constants 0x02/0x05 are wrong — CLAUDE.md gotcha #1); copy
// the 2.16 port's CPU strip-flip (rotate_strip case 2) AND mirror both touch
// axes in touch.cpp to keep them consistent.
//
// The CO5300 controller's visible window may need a column offset to center
// the 466-wide image in GRAM. 0 is correct on this module (no L/R shift seen);
// if the image looks shifted left/right, nudge this (the 1.8 368-wide panel
// used 16).
#define CO5300_COL_OFFSET  0

static Arduino_DataBus* bus = nullptr;
static Arduino_OLED*    gfx = nullptr;

void display_hal_init(void) {
    bus = new Arduino_ESP32QSPI(
        LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);

#if DISPLAY_IS_SH8601
    Serial.println("Display: SH8601 driver");
    // SH8601: (bus, rst, rotation, w, h) — no `ips` arg in this driver.
    // Passing one shifts width into `false`(=0) and blanks the panel.
    gfx = new Arduino_SH8601(
        bus, LCD_RESET, 0, LCD_WIDTH, LCD_HEIGHT);
#else
    Serial.println("Display: CO5300 driver");
    // CO5300: (bus, rst, rotation, w, h, col_off1, row_off1, col_off2, row_off2)
    gfx = new Arduino_CO5300(
        bus, LCD_RESET, 0,
        LCD_WIDTH, LCD_HEIGHT, CO5300_COL_OFFSET, 0, 0, 0);
#endif
}

void display_hal_begin(void) {
    gfx->begin();
    gfx->fillScreen(0x0000);
    gfx->setBrightness(200);
}

void display_hal_set_brightness(uint8_t level) {
    if (gfx) gfx->setBrightness(level);
}

void display_hal_fill_screen(uint16_t color) {
    if (gfx) gfx->fillScreen(color);
}

void display_hal_draw_bitmap(int32_t x, int32_t y, int32_t w, int32_t h,
                             const uint16_t* pixels) {
    if (gfx) gfx->draw16bitRGBBitmap(x, y, (uint16_t*)pixels, w, h);
}

void display_hal_tick(void) {
    // No rotation handling needed on this board.
}

void display_hal_round_area(int32_t* x1, int32_t* y1, int32_t* x2, int32_t* y2) {
    // CO5300 wants even-aligned flush regions.
    *x1 = *x1 & ~1;
    *y1 = *y1 & ~1;
    *x2 = *x2 | 1;
    *y2 = *y2 | 1;
}
