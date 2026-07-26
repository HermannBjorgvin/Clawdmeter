#pragma once
#include <stdint.h>

// Minimal SSD1681 e-paper driver. Board-private to waveshare_epaper_154;
// not exposed in hal/. Vendored to avoid pulling in a multi-panel
// dependency for a single panel target. Mirrors the AMOLED-1.8 board's
// vendored FT3168 touch reader.
//
// Framebuffer is a 1bpp packed bitmap of LCD_WIDTH x LCD_HEIGHT pixels,
// MSB-first within each byte, stored row-major. Bit value 1 = white,
// 0 = black (SSD1681 convention).

// Bring up SPI, reset the panel, and load the full-refresh waveform LUT.
// Call once at boot, and again before any epd_full_refresh after a
// preceding epd_init_partial (the partial-init sequence overwrites the
// full LUT in chip RAM).
void epd_init(void);

// Reset the panel into partial-refresh mode: loads the partial-mode
// LUT and the 0x37 / border / 0x22 0xC0 setup pass that the SSD1681
// requires before display update mode 2 (cmd 0x22 0xCF) will produce
// clean output. Must be called after a full refresh has seeded the
// chip's "previous" RAM (cmd 0x26) — epd_full_refresh does that
// automatically.
void epd_init_partial(void);

// Push the entire framebuffer with a full refresh (~1.5 s, flashes the
// panel to clear ghost pixels). Also seeds the SSD1681's "previous" RAM
// (cmd 0x26) with the same image so the next epd_partial_refresh has a
// correct diff baseline.
void epd_full_refresh(const uint8_t* framebuf);

// Push the entire framebuffer with a partial refresh (~300 ms, no
// flash). The SSD1681 computes the diff against its "previous" RAM
// internally, so the caller does not need a dirty rect; the arguments
// are kept for API symmetry and currently ignored. Requires that
// epd_init_partial has been called since the most recent
// epd_full_refresh — display_hal_tick handles that sequencing.
void epd_partial_refresh(const uint8_t* framebuf,
                         int x1, int y1, int x2, int y2);

// Put the panel into deep-sleep mode 1 (retains RAM contents).
void epd_deep_sleep(void);
