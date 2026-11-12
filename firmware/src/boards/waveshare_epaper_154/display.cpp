#include "../../hal/display_hal.h"
#include "../../hal/mono_panel.h"
#include "board.h"
#include "epd.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <string.h>

// 200x200 / 8 = 5000-byte 1bpp framebuffer (MSB-first, row-major).
#define FB_BYTES  ((LCD_WIDTH * LCD_HEIGHT) / 8)

// Coalesce multi-region LVGL flushes from the same animation tick.
// Must be SHORTER than the e-paper UI cadences (1s spinner, 5s message)
// so settle reliably fires between ticks.
#define SETTLE_MS  100

// Full refresh cadence — purely time-based. Every on-screen value update
// (progress bar, usage %, rotating message, spinner) renders via a fast
// partial refresh at its native frequency; the only full refresh — the
// visible inversion flicker — fires on this timer solely to clear any
// accumulated ghosting. Set to 10 min to spare the panel: full refreshes
// stress the e-paper film far more than partials, so doing fewer of them
// is gentler on display lifetime. (A 250-partial count previously forced
// a full every ~4 min on the usage screen; removed — the timer alone
// governs the cadence now.)
#define FULL_REFRESH_INTERVAL_MS  600000  // 10 min — time-based ghost clearance

// Board-local partial-refresh rate limit. Replaces the epaper branch's shared
// slow_refresh cadence: LVGL animates into RAM at full speed, but the SSD1681
// (which blocks loop() on wait_busy ~300ms) commits at most one partial per
// second, preventing panel thrash and loop() stalls.
#define MIN_PARTIAL_MS  1000

static uint8_t* framebuf = nullptr;
static bool     dirty = false;
static uint32_t last_flush_ms = 0;
static uint32_t last_full_refresh_ms = 0;
static uint32_t last_partial_ms = 0;
static int      dirty_x1 = 0, dirty_y1 = 0, dirty_x2 = 0, dirty_y2 = 0;

// Track whether the SSD1681 is currently in partial-refresh mode. Set
// to true after epd_init_partial finishes, cleared when we decide it's
// time for another ghost-clearing full refresh (and epd_init re-runs to
// reload the full-refresh LUT).
static bool partial_mode = false;

static inline void expand_dirty(int x, int y, int w, int h) {
    int x2 = x + w - 1;
    int y2 = y + h - 1;
    if (!dirty) {
        dirty_x1 = x;  dirty_y1 = y;
        dirty_x2 = x2; dirty_y2 = y2;
        dirty = true;
    } else {
        if (x  < dirty_x1) dirty_x1 = x;
        if (y  < dirty_y1) dirty_y1 = y;
        if (x2 > dirty_x2) dirty_x2 = x2;
        if (y2 > dirty_y2) dirty_y2 = y2;
    }
}

void display_hal_init(void) {
    framebuf = (uint8_t*)heap_caps_malloc(FB_BYTES, MALLOC_CAP_INTERNAL);
    if (framebuf) memset(framebuf, 0xFF, FB_BYTES);  // start all-white
    epd_init();
}

void display_hal_begin(void) {
    if (framebuf) {
        // Boot full refresh: paints the panel all-white (framebuf was
        // memset to 0xFF in display_hal_init) and seeds the previous
        // RAM. Then switch the chip into partial mode so the first UI
        // tick can do a fast partial refresh instead of a redundant
        // second full refresh.
        epd_full_refresh(framebuf);
        last_full_refresh_ms = millis();
        epd_init_partial();
        partial_mode = true;
    }
}

void display_hal_set_brightness(uint8_t level) {
    (void)level;   // e-paper has no backlight.
}

void display_hal_draw_bitmap(int32_t x, int32_t y, int32_t w, int32_t h,
                             const uint16_t* pixels) {
    if (!framebuf || !pixels) return;
    for (int32_t row = 0; row < h; row++) {
        int32_t py = y + row;
        if (py < 0 || py >= LCD_HEIGHT) continue;
        for (int32_t col = 0; col < w; col++) {
            int32_t px = x + col;
            if (px < 0 || px >= LCD_WIDTH) continue;
            uint8_t bit  = 1 << (7 - (px & 7));
            uint8_t* byte = &framebuf[py * (LCD_WIDTH / 8) + (px >> 3)];
            // INVERTED for e-paper: the shared UI uses a dark theme
            // (THEME_BG=0x000000, THEME_TEXT=0xfaf9f5) which is natural
            // on AMOLED but unnatural on e-paper. Map LVGL-light pixels
            // (text/icons) to panel-black, and LVGL-dark pixels
            // (background) to panel-white — gives black-on-white output
            // on the panel without touching shared UI code. The threshold
            // is the mono_panel rule (hal/mono_panel.h): luminance 64 or
            // more is ink, between the theme's brightest background (the
            // bar track, 40) and its darkest foreground (red, 94).
            if (mono_ink565(pixels[row * w + col])) *byte &= ~bit;
            else                                    *byte |=  bit;
        }
    }
    expand_dirty(x, y, w, h);
    last_flush_ms = millis();
}

void display_hal_tick(void) {
    if (!dirty || !framebuf) return;
    if (millis() - last_flush_ms < SETTLE_MS) return;

    // Clamp dirty region into panel bounds (kept for SETTLE_MS coalescing
    // and potential future per-region partial; the SSD1681 itself
    // computes the diff internally for partial refreshes).
    if (dirty_x1 < 0) dirty_x1 = 0;
    if (dirty_y1 < 0) dirty_y1 = 0;
    if (dirty_x2 >= LCD_WIDTH)  dirty_x2 = LCD_WIDTH - 1;
    if (dirty_y2 >= LCD_HEIGHT) dirty_y2 = LCD_HEIGHT - 1;

    uint32_t now = millis();
    bool time_for_full = (now - last_full_refresh_ms) >= FULL_REFRESH_INTERVAL_MS;

    if (!partial_mode || time_for_full) {
        // Cold start or the 10-min ghost-clear timer: redo a full refresh
        // (loads full LUT + drives all pixels through the inversion
        // sequence + seeds previous RAM) and then switch the chip back
        // into partial mode for fast, flicker-free updates until the
        // timer next expires.
        epd_init();
        epd_full_refresh(framebuf);
        epd_init_partial();
        partial_mode = true;
        last_full_refresh_ms = now;
        // Seed the partial clock so a partial doesn't fire immediately
        // after this full refresh.
        last_partial_ms = now;
    } else {
        // Rate-limit partials to one per MIN_PARTIAL_MS. Return WITHOUT
        // clearing `dirty` so the latest framebuffer is committed on a
        // later eligible tick — never lose the update.
        if (now - last_partial_ms < MIN_PARTIAL_MS) return;
        epd_partial_refresh(framebuf, dirty_x1, dirty_y1, dirty_x2, dirty_y2);
        last_partial_ms = now;
    }
    dirty = false;
}

void display_hal_round_area(int32_t* x1, int32_t* y1, int32_t* x2, int32_t* y2) {
    // SSD1681 addresses pixel columns in 8-pixel bytes.
    *x1 = (*x1) & ~7;
    *x2 = (*x2) | 7;
    if (*x1 < 0) *x1 = 0;
    if (*x2 >= LCD_WIDTH) *x2 = LCD_WIDTH - 1;
}
