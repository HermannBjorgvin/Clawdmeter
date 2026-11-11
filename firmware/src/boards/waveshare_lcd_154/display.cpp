#include "../../hal/display_hal.h"
#include "../../brightness.h"
#include "board.h"
#include "rotation.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <lvgl.h>

// ST7789 over plain 4-wire SPI — the first non-QSPI panel in the tree, but
// Arduino_GFX abstracts the bus so the HAL surface is identical. The ST7789
// has no in-panel brightness command; brightness is a LEDC PWM duty on the
// backlight GPIO instead, which keeps the idle fade working unchanged.

static Arduino_DataBus* bus = nullptr;
static Arduino_ST7789*  gfx = nullptr;
static volatile uint8_t applied_rotation = LCD_ROTATION & 3;  // MADCTL state

uint8_t lcd_applied_rotation(void) { return applied_rotation; }

void display_hal_init(void) {
    bus = new Arduino_ESP32SPI(LCD_DC, LCD_CS, LCD_SCLK, LCD_MOSI,
                               GFX_NOT_DEFINED /* no MISO */);
    // 240x240 window of the 240x320 GRAM: rows 0-239 at rotation 0, so
    // offsets are 0,0 (the 80-row offset only matters for rotations 2/3;
    // Arduino_TFT::setRotation picks the right offset pair per rotation).
    // ips=true — this module needs color inversion (matches the
    // hardware-tested BambuHelper config's USE_ST7789_INVERT).
    gfx = new Arduino_ST7789(bus, LCD_RST, LCD_ROTATION, true /* ips */,
                             LCD_WIDTH, LCD_HEIGHT, 0, 0, 0, 80);
}

void display_hal_begin(void) {
    gfx->begin(80000000);   // 80 MHz write clock, hardware-tested on this panel
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

// Auto-rotation: the ST7789 rotates in hardware (MADCTL row/column exchange),
// so unlike the CO5300 boards there is no CPU pixel remap. On an orientation
// change: blank the backlight, switch MADCTL, force a full LVGL redraw at the
// new orientation, then ramp back to the user's brightness over ~100 ms.
// Called from loop() between lv_timer_handler() runs, so no flush is in
// flight while MADCTL changes.
void display_hal_tick(void) {
#if LCD_AUTO_ROTATE
    static uint8_t  ramp_step = 0;                  // 0=idle, 1..4=ramping
    static uint32_t ramp_last = 0;
    if (!gfx) return;

    uint8_t rot = lcd_effective_rotation();
    if (rot != applied_rotation) {
        display_hal_set_brightness(0);
        gfx->setRotation(rot);
        applied_rotation = rot;
        lv_obj_invalidate(lv_screen_active());
        ramp_step = 1;
        ramp_last = millis();
        return;
    }

    if (ramp_step == 0) return;
    uint32_t now = millis();
    if (now - ramp_last < 25) return;
    ramp_last = now;

    static const uint8_t pct[] = {30, 60, 85, 100};
    uint8_t target = brightness_get();
    display_hal_set_brightness((uint8_t)(((uint16_t)target * pct[ramp_step - 1]) / 100));
    if (ramp_step >= 4) ramp_step = 0;
    else                ramp_step++;
#endif
}

// ST7789 over SPI has no flush-region alignment requirement.
void display_hal_round_area(int32_t* x1, int32_t* y1, int32_t* x2, int32_t* y2) {
    (void)x1; (void)y1; (void)x2; (void)y2;
}
