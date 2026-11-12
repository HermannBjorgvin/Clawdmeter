#include "epd.h"
#include "board.h"
#include <Arduino.h>
#include <SPI.h>

// 1bpp framebuffer size: 200 x 200 / 8 = 5000 bytes
#define FB_BYTES  ((LCD_WIDTH * LCD_HEIGHT) / 8)

// Arduino Core 3.x (pioarduino) removed the VSPI/HSPI aliases; use FSPI
// (the secondary SPI peripheral on S3) since the default SPI object is
// already in use by the framework. FSPI maps to the same peripheral as
// the old VSPI on S3.
static SPIClass         spi_bus(FSPI);
static SPISettings      spi_cfg(4000000, MSBFIRST, SPI_MODE0);

// ---------------------------------------------------------------------------
// Waveform LUTs for the Waveshare 1.54" V2 SSD1681 panel
// ---------------------------------------------------------------------------
// Both arrays are 159 bytes: the first 153 go to cmd 0x32; the remaining
// 6 are trailer settings for cmds 0x3F, 0x03, 0x04 (3 bytes), and 0x2C.
// Sourced verbatim from waveshareteam/ESP32-S3-ePaper-1.54
// (02_Example/Arduino/07_BATT_PWR_Test/src/display/epaper_driver_bsp.cpp,
// symbols WF_Full_1IN54 and WF_PARTIAL_1IN54_0). Loading the panel-
// specific LUT explicitly is mandatory — the chip's OTP LUT is
// unreliable on this revision.
static const uint8_t WF_FULL[159] = {
    0x80,0x48,0x40,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x40,0x48,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x80,0x48,0x40,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x40,0x48,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x0A,0x00,0x00,0x00,0x00,0x00,0x00,
    0x08,0x01,0x00,0x08,0x01,0x00,0x02,
    0x0A,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x22,0x22,0x22,0x22,0x22,0x22,0x00,0x00,0x00,
    0x22,0x17,0x41,0x00,0x32,0x20
};

static const uint8_t WF_PARTIAL[159] = {
    0x00,0x40,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x80,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x40,0x40,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x0F,0x00,0x00,0x00,0x00,0x00,0x00,
    0x01,0x01,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x22,0x22,0x22,0x22,0x22,0x22,0x00,0x00,0x00,
    0x02,0x17,0x41,0xB0,0x32,0x28
};

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

// Poll EPD_BUSY until LOW (idle). SSD1681 signals busy = HIGH.
// Returns true if idle; false on 5-second timeout.
static bool wait_busy(void) {
    uint32_t deadline = millis() + 5000;
    while (digitalRead(EPD_BUSY) == HIGH) {
        if (millis() > deadline) return false;
        delay(1);
    }
    return true;
}

// Send a command byte (DC = LOW).
static void write_cmd(uint8_t cmd) {
    digitalWrite(EPD_DC, LOW);
    spi_bus.beginTransaction(spi_cfg);
    digitalWrite(EPD_CS, LOW);
    spi_bus.transfer(cmd);
    digitalWrite(EPD_CS, HIGH);
    spi_bus.endTransaction();
}

// Send a single data byte (DC = HIGH).
static void write_data(uint8_t data) {
    digitalWrite(EPD_DC, HIGH);
    spi_bus.beginTransaction(spi_cfg);
    digitalWrite(EPD_CS, LOW);
    spi_bus.transfer(data);
    digitalWrite(EPD_CS, HIGH);
    spi_bus.endTransaction();
}

// Send a multi-byte data buffer (DC = HIGH).
static void write_data_n(const uint8_t* buf, size_t len) {
    if (!buf || len == 0) return;
    digitalWrite(EPD_DC, HIGH);
    spi_bus.beginTransaction(spi_cfg);
    digitalWrite(EPD_CS, LOW);
    spi_bus.writeBytes(buf, len);
    digitalWrite(EPD_CS, HIGH);
    spi_bus.endTransaction();
}

// Reference's EPD_SetWindows: X is divided by 8 (byte addressing),
// Y is sent as 16-bit pixel address. Args are pixel coordinates.
static void set_window_px(uint16_t x_start_px, uint16_t y_start_px,
                          uint16_t x_end_px,   uint16_t y_end_px) {
    write_cmd(0x44);
    write_data((x_start_px >> 3) & 0xFF);
    write_data((x_end_px   >> 3) & 0xFF);

    write_cmd(0x45);
    write_data( y_start_px       & 0xFF);
    write_data((y_start_px >> 8) & 0xFF);
    write_data( y_end_px         & 0xFF);
    write_data((y_end_px   >> 8) & 0xFF);
}

// Reference's EPD_SetCursor: X is 8-bit (byte position), Y is 16-bit.
// Args are pixel coordinates; X gets divided by 8.
static void set_cursor_px(uint16_t x_px, uint16_t y_px) {
    write_cmd(0x4E);
    write_data((x_px >> 3) & 0xFF);

    write_cmd(0x4F);
    write_data( y_px       & 0xFF);
    write_data((y_px >> 8) & 0xFF);
}

// Load a 159-byte waveform LUT. Mirrors reference's EPD_SetLut:
// 153 bytes via cmd 0x32, then 6 trailer bytes to cmds 0x3F, 0x03,
// 0x04 (×3), 0x2C.
static void load_lut(const uint8_t* lut) {
    write_cmd(0x32);
    write_data_n(lut, 153);
    wait_busy();

    write_cmd(0x3F); write_data(lut[153]);
    write_cmd(0x03); write_data(lut[154]);
    write_cmd(0x04); write_data(lut[155]); write_data(lut[156]); write_data(lut[157]);
    write_cmd(0x2C); write_data(lut[158]);
}

// Drive the panel reset pin with the timing the Waveshare reference
// uses for both EPD_Init and EPD_Init_Partial: HIGH 50 / LOW 20 /
// HIGH 50 ms, then wait for BUSY to clear.
static void hardware_reset(void) {
    digitalWrite(EPD_RST, HIGH); delay(50);
    digitalWrite(EPD_RST, LOW);  delay(20);
    digitalWrite(EPD_RST, HIGH); delay(50);
    wait_busy();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void epd_init(void) {
    // First-call setup: enable regulator + bring up SPI + configure pins.
    // On re-entry (e.g. after epd_init_partial when display_hal_tick wants
    // to do a full refresh again) these are idempotent and cheap.
    pinMode(EPD_PWR, OUTPUT);
    digitalWrite(EPD_PWR, LOW);   // active-LOW: drive LOW to enable
    delay(10);                    // let Vcc settle before clocking SCLK/CS

    spi_bus.begin(EPD_SCLK, /*MISO=*/-1, EPD_MOSI, /*SS=*/-1);

    pinMode(EPD_CS,   OUTPUT);  digitalWrite(EPD_CS,   HIGH);
    pinMode(EPD_DC,   OUTPUT);  digitalWrite(EPD_DC,   HIGH);
    pinMode(EPD_RST,  OUTPUT);  digitalWrite(EPD_RST,  HIGH);
    pinMode(EPD_BUSY, INPUT);

    hardware_reset();

    write_cmd(0x12);             // SW reset
    wait_busy();

    // Driver output control — A[8:0]=LCD_HEIGHT-1, B[2:0]=0x01 (TB=1).
    write_cmd(0x01);
    write_data((LCD_HEIGHT - 1) & 0xFF);
    write_data(((LCD_HEIGHT - 1) >> 8) & 0xFF);
    write_data(0x01);

    // Data entry mode 0x01: X+ increment, Y- decrement, X-major.
    write_cmd(0x11);
    write_data(0x01);

    // Reference: EPD_SetWindows(0, Width-1, Height-1, 0)
    set_window_px(0, LCD_HEIGHT - 1, LCD_WIDTH - 1, 0);

    // Border waveform — reference value for full-refresh init.
    write_cmd(0x3C);
    write_data(0x01);

    // Use internal temperature sensor.
    write_cmd(0x18);
    write_data(0x80);

    // Load temperature + waveform from OTP, then activate. The reference
    // does this even though the OTP LUT will get overridden by load_lut
    // below — skipping it left some panels in an indeterminate state.
    write_cmd(0x22);
    write_data(0xB1);
    write_cmd(0x20);

    set_cursor_px(0, LCD_HEIGHT - 1);
    wait_busy();

    // Explicitly load the panel-specific full-refresh LUT.
    load_lut(WF_FULL);
}

void epd_init_partial(void) {
    // Reference's EPD_Init_Partial runs its own reset + LUT load sequence
    // and finishes with a 0x22 0xC0 + 0x20 master activation that puts
    // the SSD1681 into partial-display-mode-ready state. After this call
    // the chip will respond to cmd 0x22 0xCF as a partial refresh against
    // its current "previous" RAM (cmd 0x26) baseline.
    hardware_reset();

    load_lut(WF_PARTIAL);

    // 10-byte 0x37 parameters — opaque, vendored verbatim from reference.
    write_cmd(0x37);
    write_data(0x00); write_data(0x00); write_data(0x00); write_data(0x00); write_data(0x00);
    write_data(0x40); write_data(0x00); write_data(0x00); write_data(0x00); write_data(0x00);

    // Partial-mode border waveform.
    write_cmd(0x3C);
    write_data(0x80);

    // Display update control 2 with 0xC0 + master activation: enables
    // clock+analog, sets up partial mode, then disables — leaves the
    // chip ready for cmd 0x22 0xCF triggered refreshes.
    write_cmd(0x22);
    write_data(0xC0);
    write_cmd(0x20);
    wait_busy();
}

void epd_full_refresh(const uint8_t* framebuf) {
    set_window_px(0, LCD_HEIGHT - 1, LCD_WIDTH - 1, 0);
    set_cursor_px(0, LCD_HEIGHT - 1);

    // Write to B/W RAM (cmd 0x24) — the visible buffer.
    write_cmd(0x24);
    write_data_n(framebuf, FB_BYTES);

    // Also seed the "previous" RAM (cmd 0x26) with the same image. The
    // SSD1681 uses 0x26 as the diff baseline for subsequent partial
    // refreshes (mirrors the reference's EPD_DisplayPartBaseImage). For
    // a pure full refresh this is a small waste of SPI time but ensures
    // partial mode can take over cleanly immediately afterward.
    set_cursor_px(0, LCD_HEIGHT - 1);
    write_cmd(0x26);
    write_data_n(framebuf, FB_BYTES);

    // Display update control 2 = 0xC7: clock+analog+display(mode 1)
    // +disable. No "load LUT" bit — the LUT was loaded in epd_init()
    // and persists. Matches reference's EPD_TurnOnDisplay.
    write_cmd(0x22);
    write_data(0xC7);
    write_cmd(0x20);
    wait_busy();
}

void epd_partial_refresh(const uint8_t* framebuf,
                         int x1, int y1, int x2, int y2) {
    // The SSD1681 computes the diff against cmd-0x26 RAM internally, so
    // we send the full framebuffer regardless of the caller's dirty
    // rect. Keeping the bounds args for API symmetry / future use.
    (void)x1; (void)y1; (void)x2; (void)y2;

    set_window_px(0, LCD_HEIGHT - 1, LCD_WIDTH - 1, 0);
    set_cursor_px(0, LCD_HEIGHT - 1);

    write_cmd(0x24);
    write_data_n(framebuf, FB_BYTES);

    // Display update control 2 = 0xCF: clock+analog+display(mode 2 /
    // partial)+disable. Requires epd_init_partial to have set up the
    // partial LUT and the 0x37 parameters since the last reset.
    // Matches reference's EPD_TurnOnDisplayPart.
    write_cmd(0x22);
    write_data(0xCF);
    write_cmd(0x20);
    wait_busy();
}

void epd_deep_sleep(void) {
    write_cmd(0x10);                // deep sleep mode
    write_data(0x01);               //   mode 1: retain RAM contents
}
