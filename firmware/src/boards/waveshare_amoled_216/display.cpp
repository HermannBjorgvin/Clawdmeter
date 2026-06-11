#include "../../hal/display_hal.h"
#include "../../hal/imu_hal.h"
#include "../../brightness.h"
#include "board.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <esp_heap_caps.h>
#include <lvgl.h>

// Render strip used when rotating in software. Sized to the largest LVGL
// partial flush we ever do (LCD_WIDTH × BUF_LINES, set in main.cpp).
#define ROT_BUF_LINES 40
static uint16_t* rot_buf = nullptr;

static Arduino_DataBus* bus = nullptr;
static Arduino_CO5300*  gfx = nullptr;

void display_hal_init(void) {
    bus = new Arduino_ESP32QSPI(
        LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
    // CO5300 constructor: (bus, rst, rotation, w, h, col_offset1..2, row_offset1..2)
    gfx = new Arduino_CO5300(
        bus, LCD_RESET, 0 /* rotation handled in software */,
        LCD_WIDTH, LCD_HEIGHT, 0, 0, 0, 0);
}

void display_hal_begin(void) {
    gfx->begin();
    gfx->fillScreen(0x0000);
    gfx->setBrightness(200);

    // Allocate rotation strip (PSRAM). Sized to match main.cpp's BUF_LINES.
    rot_buf = (uint16_t*)heap_caps_malloc(LCD_WIDTH * ROT_BUF_LINES * 2, MALLOC_CAP_SPIRAM);
}

void display_hal_set_brightness(uint8_t level) {
    if (gfx) gfx->setBrightness(level);
}

void display_hal_fill_screen(uint16_t color) {
    if (gfx) gfx->fillScreen(color);
}

// Rotate a w×h strip into rot_buf and compute destination coordinates on the
// 480×480 panel. Src is row-major over the rectangle (sx, sy, w, h).
static void rotate_strip(const uint16_t* src, int32_t w, int32_t h,
                         int32_t sx, int32_t sy, uint8_t r,
                         int32_t* dx, int32_t* dy, int32_t* dw, int32_t* dh) {
    const int S = LCD_WIDTH;

    switch (r) {
    case 1: // 90° CW: (x,y) -> (S-1-y, x)
        *dw = h; *dh = w;
        *dx = S - sy - h;
        *dy = sx;
        for (int32_t y = 0; y < h; y++) {
            for (int32_t x = 0; x < w; x++) {
                rot_buf[x * h + (h - 1 - y)] = src[y * w + x];
            }
        }
        break;
    case 2: // 180°: (x,y) -> (S-1-x, S-1-y)
        *dw = w; *dh = h;
        *dx = S - sx - w;
        *dy = S - sy - h;
        for (int32_t y = 0; y < h; y++) {
            for (int32_t x = 0; x < w; x++) {
                rot_buf[(h - 1 - y) * w + (w - 1 - x)] = src[y * w + x];
            }
        }
        break;
    case 3: // 270° CW: (x,y) -> (y, S-1-x)
        *dw = h; *dh = w;
        *dx = sy;
        *dy = S - sx - w;
        for (int32_t y = 0; y < h; y++) {
            for (int32_t x = 0; x < w; x++) {
                rot_buf[(w - 1 - x) * h + y] = src[y * w + x];
            }
        }
        break;
    default:
        *dx = sx; *dy = sy; *dw = w; *dh = h;
        break;
    }
}

void display_hal_draw_bitmap(int32_t x, int32_t y, int32_t w, int32_t h,
                             const uint16_t* pixels) {
    if (!gfx) return;
    uint8_t r = imu_hal_rotation_quadrant();
    if (r == 0 || !rot_buf) {
        gfx->draw16bitRGBBitmap(x, y, (uint16_t*)pixels, w, h);
        return;
    }
    int32_t dx, dy, dw, dh;
    rotate_strip(pixels, w, h, x, y, r, &dx, &dy, &dw, &dh);
    gfx->draw16bitRGBBitmap(dx, dy, rot_buf, dw, dh);
}

// On rotation change, blank the panel, force a full LVGL redraw at the new
// orientation, then ramp brightness back up over ~125ms so the transition
// reads as deliberate.
void display_hal_tick(void) {
    static uint8_t  last_rotation = 0;
    static uint8_t  ramp_step = 0;     // 0=idle, 1..4=ramping
    static uint32_t ramp_last = 0;

    uint8_t rot = imu_hal_rotation_quadrant();
    if (rot != last_rotation) {
        display_hal_set_brightness(0);
        last_rotation = rot;
        lv_obj_invalidate(lv_screen_active());
        ramp_step = 1;
        return;
    }

    if (ramp_step == 0) return;
    uint32_t now = millis();
    if (now - ramp_last < 25) return;
    ramp_last = now;

    // Ramp back to the user's chosen brightness (not a hardcoded level), so a
    // physical rotation doesn't reset what they set via PWR-short-press.
    static const uint8_t pct[] = {30, 60, 85, 100};
    uint8_t target = brightness_get();
    display_hal_set_brightness((uint8_t)(((uint16_t)target * pct[ramp_step - 1]) / 100));
    if (ramp_step >= 4) ramp_step = 0;
    else                ramp_step++;
}

// CO5300 requires even-aligned flush regions.
void display_hal_round_area(int32_t* x1, int32_t* y1, int32_t* x2, int32_t* y2) {
    *x1 = *x1 & ~1;
    *y1 = *y1 & ~1;
    *x2 = *x2 | 1;
    *y2 = *y2 | 1;
}
