#include "../../hal/display_hal.h"
#include "../../hal/imu_hal.h"
#include "../../brightness.h"
#include "board.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <lvgl.h>

// C6 AMOLED-2.16 uses a CO5300 AMOLED panel (per the Waveshare
// ESP32-C6-Touch-AMOLED-2.16 spec) — the same controller as the S3
// AMOLED-2.16 sibling, so we drive it with Arduino_CO5300 and reuse that
// class's vendor-correct init rather than the SH8601 class + a hand-patched
// sequence. LCD reset is not wired to any MCU GPIO; the panel boots from its
// internal power-on reset (rst = GFX_NOT_DEFINED). Auto-rotation is done by
// the panel itself through MADCTL (no CPU rotation strip, so no RAM cost):
// each IMU quadrant maps to a MADCTL value below and LVGL simply redraws.

static Arduino_DataBus* bus = nullptr;
static Arduino_CO5300*  gfx = nullptr;

void display_hal_init(void) {
    bus = new Arduino_ESP32QSPI(
        LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
    // CO5300 constructor: (bus, rst, rotation, w, h, col_off1..2, row_off1..2).
    // No reset GPIO on this board; the 480-wide panel is full-width so all
    // offsets are 0 — matches the S3 AMOLED-2.16 instantiation.
    gfx = new Arduino_CO5300(
        bus, GFX_NOT_DEFINED, 0 /* rotation disabled */,
        LCD_WIDTH, LCD_HEIGHT, 0, 0, 0, 0);
}

// Arduino_CO5300::begin() already issues SLPOUT, SPI-mode control, pixel
// format, brightness-control, DISPON and a default MADCTL. The ONLY thing it
// does not set is this panel's manufacturer page-0x20 driving-voltage
// registers (0x19/0x1C) — without them the panel stays black even with the
// rails up. Set just those; everything else the SH8601-era hack also wrote
// (0xC4/0x53/0x51/0x63/0x29) is now covered by the class init, and we override
// MADCTL below to fix orientation.
// The CO5300 class default (rotation-0, MADCTL 0x00) leaves the panel
// sideways on this board — confirmed on hardware. Restore the MV+ML
// transpose (MADCTL 0x30) that the pre-CO5300-rebase SH8601-hack version
// used to write; touch mapping in touch.cpp is calibrated to match.
static void send_panel_driving_init(Arduino_DataBus* b) {
    b->beginWrite();
    b->writeC8D8(0xFE, 0x20);    // enter manufacturer command page 0x20
    b->writeC8D8(0x19, 0x10);    // panel driving voltage
    b->writeC8D8(0x1C, 0xA0);    // panel driving voltage
    b->writeC8D8(0xFE, 0x00);    // back to user command page
    b->writeC8D8(0x36, 0x30);    // MADCTL: MV transpose + ML (orientation fix)
    b->endWrite();
    delay(20);
}

void display_hal_begin(void) {
    gfx->begin();
    send_panel_driving_init(bus);   // panel-specific regs the class init omits
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

// MADCTL per IMU quadrant. Base orientation is MV|ML (0x30, see
// send_panel_driving_init). A 90° step toggles MV and mirrors one axis;
// 180° mirrors both. ML (0x10) is kept in all four.
// Measured on hardware: upright on the desk stand the IMU reads gravity on +Y
// (quadrant 3), and that is the orientation the base MADCTL (0x30) is tuned
// for. 0x90 rotates the image 90° CCW relative to base, 0x50 90° CW, 0xF0 180°.
//   q3 (upright):        MV|ML         0x30
//   q1 (upside down):    MV|MX|MY|ML   0xF0
//   q2 / q0 (on a side): MY|ML 0x90 / MX|ML 0x50
static const uint8_t MADCTL_BY_QUADRANT[4] = { 0x50, 0xF0, 0x90, 0x30 };

static void apply_rotation(uint8_t q) {
    if (!bus) return;
    bus->beginWrite();
    bus->writeC8D8(0x36, MADCTL_BY_QUADRANT[q & 3]);
    bus->endWrite();
}

// On rotation change: blank, switch MADCTL, force a full LVGL redraw at the
// new orientation, then ramp brightness back up over ~125 ms (same feel as
// the S3 port).
void display_hal_tick(void) {
    static uint8_t  last_rotation = 0;
    static uint8_t  ramp_step = 0;     // 0=idle, 1..4=ramping
    static uint32_t ramp_last = 0;

    uint8_t rot = imu_hal_rotation_quadrant();
    if (rot != last_rotation) {
        display_hal_set_brightness(0);
        last_rotation = rot;
        apply_rotation(rot);
        lv_obj_invalidate(lv_screen_active());
        ramp_step = 1;
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
}

// CO5300 requires even-aligned flush regions.
void display_hal_round_area(int32_t* x1, int32_t* y1, int32_t* x2, int32_t* y2) {
    *x1 = *x1 & ~1;
    *y1 = *y1 & ~1;
    *x2 = *x2 | 1;
    *y2 = *y2 | 1;
}
