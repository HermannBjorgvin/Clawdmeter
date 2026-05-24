#include "../../hal/display_hal.h"
#include "../../hal/imu_hal.h"
#include "board.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <esp_heap_caps.h>
#include <lvgl.h>

// Render strip used when rotating in software. Sized to the largest LVGL
// partial flush we ever do (max(LCD_W, LCD_H) × BUF_LINES). Larger than
// strictly necessary in portrait (where width=410); sized for landscape
// so we don't reallocate on rotation.
#define ROT_BUF_LINES 40
static uint16_t* rot_buf = nullptr;

static Arduino_DataBus* bus = nullptr;
static Arduino_CO5300*  gfx = nullptr;

// Set true once per rotation transition by display_hal_tick(); drained
// to main.cpp via display_hal_consume_rotation_change() so shared code
// owns the lv_display_set_resolution + ui_rebuild() reaction.
static bool rotation_change_pending = false;

void display_hal_init(void) {
    bus = new Arduino_ESP32QSPI(
        LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
    // CO5300 constructor: (bus, rst, rotation, w, h, col_offset1, row_offset1, col_offset2, row_offset2)
    // LCD_RESET is a direct GPIO on this board (no IO expander).
    //
    // col_offset = 22: the 410-px visible panel sits at controller column
    // 22..431 (out of 432 RAM columns). Without this offset the rightmost
    // 22 panel columns show stale/uninitialised content (a green strip on
    // AMOLED). Confirmed from the official ESP-IDF BSP, which sends
    // `2A 00 16 01 AF` (column window 22..431) and `2B 00 00 01 F5` (row
    // window 0..501) at init.
    gfx = new Arduino_CO5300(
        bus, LCD_RESET, 0 /* rotation handled in software */,
        LCD_WIDTH, LCD_HEIGHT, 22, 0, 22, 0);
}

void display_hal_begin(void) {
    gfx->begin();
    gfx->fillScreen(0x0000);
    gfx->setBrightness(200);

    // Allocate rotation strip (PSRAM). Sized to match the largest possible
    // LVGL flush width in either orientation (max(W,H) * BUF_LINES * 2).
    const int max_w = (LCD_WIDTH > LCD_HEIGHT) ? LCD_WIDTH : LCD_HEIGHT;
    rot_buf = (uint16_t*)heap_caps_malloc(max_w * ROT_BUF_LINES * 2, MALLOC_CAP_SPIRAM);
}

void display_hal_set_brightness(uint8_t level) {
    if (gfx) gfx->setBrightness(level);
}

void display_hal_fill_screen(uint16_t color) {
    if (gfx) gfx->fillScreen(color);
}

// Rotate a w×h strip into rot_buf and compute destination coordinates on
// the native 410×502 panel. (x_l, y_l) are LVGL coordinates in the
// *current* orientation: in landscape the LVGL canvas is 502×410, so
// 0 ≤ x_l < 502 and 0 ≤ y_l < 410. Destination coordinates address the
// physical raster (always 410×502 regardless of rotation). Independent
// native_w and native_h are essential here — using a single S=W or S=H
// (the 2.16's old pattern) would slice content off the panel.
static void rotate_strip(const uint16_t* src, int32_t w, int32_t h,
                         int32_t sx, int32_t sy, uint8_t r,
                         int32_t* dx, int32_t* dy, int32_t* dw, int32_t* dh) {
    const int32_t native_w = LCD_WIDTH;
    const int32_t native_h = LCD_HEIGHT;

    switch (r) {
    case 1: // 90° CW: (x_l, y_l) -> (native_w - 1 - y_l, x_l)
        *dw = h; *dh = w;
        *dx = native_w - sy - h;
        *dy = sx;
        for (int32_t y = 0; y < h; y++) {
            for (int32_t x = 0; x < w; x++) {
                rot_buf[x * h + (h - 1 - y)] = src[y * w + x];
            }
        }
        break;
    case 2: // 180°: (x_l, y_l) -> (native_w - 1 - x_l, native_h - 1 - y_l)
        *dw = w; *dh = h;
        *dx = native_w - sx - w;
        *dy = native_h - sy - h;
        for (int32_t y = 0; y < h; y++) {
            for (int32_t x = 0; x < w; x++) {
                rot_buf[(h - 1 - y) * w + (w - 1 - x)] = src[y * w + x];
            }
        }
        break;
    case 3: // 270° CW: (x_l, y_l) -> (y_l, native_h - 1 - x_l)
        *dw = h; *dh = w;
        *dx = sy;
        *dy = native_h - sx - w;
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

// On rotation change, blank the panel and start the 4-step brightness
// ramp back up over ~125 ms. The actual "tell LVGL the resolution
// changed and rebuild widgets" step happens in main.cpp after it drains
// display_hal_consume_rotation_change().
void display_hal_tick(void) {
    static uint8_t  last_rotation = 0;
    static uint8_t  ramp_step = 0;     // 0=idle, 1..4=ramping
    static uint32_t ramp_last = 0;

    uint8_t rot = imu_hal_rotation_quadrant();
    if (rot != last_rotation) {
        display_hal_set_brightness(0);
        last_rotation = rot;
        rotation_change_pending = true;
        ramp_step = 1;
        return;
    }

    if (ramp_step == 0) return;
    uint32_t now = millis();
    if (now - ramp_last < 25) return;
    ramp_last = now;

    static const uint8_t levels[] = {60, 120, 170, 200};
    display_hal_set_brightness(levels[ramp_step - 1]);
    if (ramp_step >= 4) ramp_step = 0;
    else                ramp_step++;
}

// Non-square panel: active dims swap on quadrants 1 and 3. Quadrant 0
// is native portrait (W=410, H=502), quadrant 1 is 90° CW (W=502, H=410),
// quadrant 2 is 180° (W=410, H=502), quadrant 3 is 270° CW (W=502, H=410).
int16_t display_hal_active_width(void) {
    uint8_t r = imu_hal_rotation_quadrant();
    return (r & 1) ? LCD_HEIGHT : LCD_WIDTH;
}

int16_t display_hal_active_height(void) {
    uint8_t r = imu_hal_rotation_quadrant();
    return (r & 1) ? LCD_WIDTH : LCD_HEIGHT;
}

bool display_hal_consume_rotation_change(void) {
    if (rotation_change_pending) {
        rotation_change_pending = false;
        return true;
    }
    return false;
}

// CO5300 requires even-aligned flush regions.
void display_hal_round_area(int32_t* x1, int32_t* y1, int32_t* x2, int32_t* y2) {
    *x1 = *x1 & ~1;
    *y1 = *y1 & ~1;
    *x2 = *x2 | 1;
    *y2 = *y2 | 1;
}
