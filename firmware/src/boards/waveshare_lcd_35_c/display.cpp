#include "../../hal/display_hal.h"
#include "../../hal/imu_hal.h"
#include "../../brightness.h"
#include "board.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <lvgl.h>

// Waveshare LCD-3.5-C: ST7796 IPS panel over 4-wire SPI, 320x480 portrait.
// The 180-degree flip is done by the panel's MADCTL register, so the frame
// stays 320x480 and no software rotation or PSRAM buffer is needed.

#define BL_PWM_FREQ   5000
#define BL_PWM_BITS   8

// MADCTL rotation: 0 = upright, 2 = upside down (MX|MY|BGR).
#define ROT_UPRIGHT   0
#define ROT_FLIPPED   2

static Arduino_DataBus* bus = nullptr;
static Arduino_GFX*     gfx = nullptr;

void display_hal_init(void) {
    bus = new Arduino_ESP32SPI(LCD_DC, LCD_CS, LCD_SCK, LCD_MOSI,
                               GFX_NOT_DEFINED);
    gfx = new Arduino_ST7796(
        bus, LCD_RST, ROT_UPRIGHT, true /*IPS*/,
        LCD_WIDTH, LCD_HEIGHT, 0, 0, 0, 0);
}

void display_hal_begin(void) {
    // Waveshare's ST7796 wiring uses SPI mode 0. Arduino GFX defaults to
    // mode 3 on ESP32, which leaves this panel backlit but command-deaf.
    static_cast<Arduino_ST7796*>(gfx)->begin(80000000, SPI_MODE0);
    gfx->fillScreen(0x0000);
    // Enable the backlight only after the panel is initialized + cleared, so
    // there's no white flash on boot. LEDC gives us real brightness control.
    ledcAttach(LCD_BL, BL_PWM_FREQ, BL_PWM_BITS);
    ledcWrite(LCD_BL, 200);
}

void display_hal_set_brightness(uint8_t level) {
    ledcWrite(LCD_BL, level);
}

void display_hal_draw_bitmap(int32_t x, int32_t y, int32_t w, int32_t h,
                             const uint16_t* pixels) {
    if (gfx) gfx->draw16bitRGBBitmap(x, y, (uint16_t*)pixels, w, h);
}

// On rotation change, blank the panel, flip MADCTL, force a full LVGL redraw,
// then ramp brightness back up over ~125ms.
void display_hal_tick(void) {
    static uint8_t  last_rotation = 0;
    static uint8_t  ramp_step = 0;     // 0=idle, 1..4=ramping
    static uint32_t ramp_last = 0;

    uint8_t rot = imu_hal_rotation_quadrant();
    if (rot != last_rotation) {
        display_hal_set_brightness(0);
        last_rotation = rot;
        static_cast<Arduino_ST7796*>(gfx)->setRotation(rot == 2 ? ROT_FLIPPED : ROT_UPRIGHT);
        lv_obj_invalidate(lv_screen_active());
        ramp_step = 1;
        return;
    }

    if (ramp_step == 0) return;
    uint32_t now = millis();
    if (now - ramp_last < 25) return;
    ramp_last = now;

    // Ramp to the user's chosen brightness, not a fixed level, so a physical
    // flip doesn't reset what they set with a PWR short press.
    static const uint8_t pct[] = {30, 60, 85, 100};
    uint8_t target = brightness_get();
    display_hal_set_brightness((uint8_t)(((uint16_t)target * pct[ramp_step - 1]) / 100));
    if (ramp_step >= 4) ramp_step = 0;
    else                ramp_step++;
}

void display_hal_round_area(int32_t* x1, int32_t* y1, int32_t* x2, int32_t* y2) {
    // ST7796 has no even-alignment constraint on flush regions.
    (void)x1; (void)y1; (void)x2; (void)y2;
}
