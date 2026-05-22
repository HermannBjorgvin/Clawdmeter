#include "../../hal/display_hal.h"
#include "board.h"
#include <Arduino.h>
#include <SPI.h>

// ----------------------------------------------------------------------
// Xingzhi Cube 1.83 panel = NV3023 (not ST7789 as the body sticker
// suggests). The xiaozhi-esphome project confirmed the chip and shared
// the exact init sequence + transform required:
//   https://github.com/RealDeco/xiaozhi-esphome/blob/main/devices/Xingzhi/xingzhi-cube-1.83-2mic.yaml
//
// We bypass Arduino_GFX entirely for this board — ST7789 init plus
// Arduino_GFX's pixel-streaming both fail on NV3023. This is a tiny
// hand-rolled MIPI-SPI driver that issues the NV3023 init commands and
// pushes RGB565 pixels with the correct column offset (36) and BGR
// color order.
// ----------------------------------------------------------------------

#define DISPLAY_SPI_HZ   40000000
#define COL_OFFSET       36                // panel starts at RAM column 36
#define ROW_OFFSET       0

static SPIClass*  spi = nullptr;
static SPISettings spi_settings(DISPLAY_SPI_HZ, MSBFIRST, SPI_MODE0);

static const int BL_PWM_FREQ_HZ = 5000;
static const int BL_PWM_RES_BITS = 8;

// ---- Low-level SPI write helpers (CS asserted by caller) ----

static inline void dc_cmd(void)  { digitalWrite(LCD_DC, LOW);  }
static inline void dc_data(void) { digitalWrite(LCD_DC, HIGH); }
static inline void cs_lo(void)   { digitalWrite(LCD_CS, LOW);  }
static inline void cs_hi(void)   { digitalWrite(LCD_CS, HIGH); }

static void spi_write_cmd(uint8_t cmd) {
    dc_cmd();
    spi->transfer(cmd);
    dc_data();
}

static void spi_write_data8(uint8_t b) {
    spi->transfer(b);
}

static void spi_write_data_bytes(const uint8_t* buf, size_t n) {
    spi->writeBytes(buf, n);
}

// ---- NV3023 init sequence (transcribed verbatim from xiaozhi-esphome) ----
struct InitCmd { uint8_t cmd; uint8_t len; const uint8_t* data; uint16_t delay_ms; };

#define INIT_BYTES(...) (const uint8_t[]){ __VA_ARGS__ }

static const uint8_t d_FD[] = {0x06, 0x08};
static const uint8_t d_61[] = {0x07, 0x04};
static const uint8_t d_62[] = {0x00, 0x44, 0x45};
static const uint8_t d_63[] = {0x41, 0x07, 0x12, 0x12};
static const uint8_t d_64[] = {0x37};
static const uint8_t d_65[] = {0x09, 0x10, 0x21};
static const uint8_t d_66[] = {0x09, 0x10, 0x21};
static const uint8_t d_67[] = {0x20, 0x40};
static const uint8_t d_68[] = {0x90, 0x4C, 0x7C, 0x66};
static const uint8_t d_B1[] = {0x0F, 0x02, 0x01};
static const uint8_t d_B4[] = {0x01};
static const uint8_t d_B5[] = {0x02, 0x02, 0x0A, 0x14};
static const uint8_t d_B6[] = {0x04, 0x01, 0x9F, 0x00, 0x02};
static const uint8_t d_DF[] = {0x11};
static const uint8_t d_E2[] = {0x13, 0x00, 0x00, 0x30, 0x33, 0x3F};
static const uint8_t d_E5[] = {0x3F, 0x33, 0x30, 0x00, 0x00, 0x13};
static const uint8_t d_E1[] = {0x00, 0x57};
static const uint8_t d_E4[] = {0x58, 0x00};
static const uint8_t d_E0[] = {0x01, 0x03, 0x0D, 0x0E, 0x0E, 0x0C, 0x15, 0x19};
static const uint8_t d_E3[] = {0x1A, 0x16, 0x0C, 0x0F, 0x0E, 0x0D, 0x02, 0x01};
static const uint8_t d_E6[] = {0x00, 0xFF};
static const uint8_t d_E7[] = {0x01, 0x04, 0x03, 0x03, 0x00, 0x12};
static const uint8_t d_E8[] = {0x00, 0x70, 0x00};
static const uint8_t d_EC[] = {0x52};
static const uint8_t d_F1[] = {0x01, 0x01, 0x02};
static const uint8_t d_F6[] = {0x09, 0x10, 0x00, 0x00};
static const uint8_t d_FD2[] = {0xFA, 0xFC};
static const uint8_t d_35[]  = {0x00};

// 0x36 (MADCTL) — orientation only, RGB color order. With LVGL
// configured for RGB565_SWAPPED and no INVON, the panel reads pixels
// directly as RGB565. MY=0x80, MV=0x20. Result: 0xA0.
static const uint8_t d_36[] = {0xA0};

// COLMOD 0x3A — 0x55 = 16-bit RGB565.
static const uint8_t d_3A[] = {0x55};

static const InitCmd init_seq[] = {
    {0xFD, 2, d_FD, 0},
    {0x61, 2, d_61, 0},
    {0x62, 3, d_62, 0},
    {0x63, 4, d_63, 0},
    {0x64, 1, d_64, 0},
    {0x65, 3, d_65, 0},
    {0x66, 3, d_66, 0},
    {0x67, 2, d_67, 0},
    {0x68, 4, d_68, 0},
    {0xB1, 3, d_B1, 0},
    {0xB4, 1, d_B4, 0},
    {0xB5, 4, d_B5, 0},
    {0xB6, 5, d_B6, 0},
    {0xDF, 1, d_DF, 0},
    {0xE2, 6, d_E2, 0},
    {0xE5, 6, d_E5, 0},
    {0xE1, 2, d_E1, 0},
    {0xE4, 2, d_E4, 0},
    {0xE0, 8, d_E0, 0},
    {0xE3, 8, d_E3, 0},
    {0xE6, 2, d_E6, 0},
    {0xE7, 6, d_E7, 0},
    {0xE8, 3, d_E8, 0},
    {0xEC, 1, d_EC, 0},
    {0xF1, 3, d_F1, 0},
    {0xF6, 4, d_F6, 0},
    {0xFD, 2, d_FD2, 0},
    {0x35, 1, d_35, 0},
    {0x36, 1, d_36, 0},                    // MADCTL — orientation + BGR
    {0x3A, 1, d_3A, 0},                    // COLMOD — RGB565
    // INVON commented out — testing whether LVGL SWAPPED removes the need.
    // {0x21, 0, nullptr, 0},
    {0x11, 0, nullptr, 200},               // SLPOUT, wait 200ms
    {0x29, 0, nullptr, 10},                // DISPON, wait 10ms
};

static void send_init_sequence(void) {
    spi->beginTransaction(spi_settings);
    cs_lo();
    for (const InitCmd& ic : init_seq) {
        spi_write_cmd(ic.cmd);
        if (ic.len > 0 && ic.data != nullptr) {
            spi_write_data_bytes(ic.data, ic.len);
        }
        if (ic.delay_ms > 0) {
            cs_hi();
            spi->endTransaction();
            delay(ic.delay_ms);
            spi->beginTransaction(spi_settings);
            cs_lo();
        }
    }
    cs_hi();
    spi->endTransaction();
}

static void set_addr_window(int32_t x, int32_t y, int32_t w, int32_t h) {
    int32_t x_start = x + COL_OFFSET;
    int32_t x_end   = x_start + w - 1;
    int32_t y_start = y + ROW_OFFSET;
    int32_t y_end   = y_start + h - 1;

    spi_write_cmd(0x2A);                   // CASET
    spi_write_data8((x_start >> 8) & 0xFF);
    spi_write_data8(x_start & 0xFF);
    spi_write_data8((x_end >> 8) & 0xFF);
    spi_write_data8(x_end & 0xFF);

    spi_write_cmd(0x2B);                   // RASET
    spi_write_data8((y_start >> 8) & 0xFF);
    spi_write_data8(y_start & 0xFF);
    spi_write_data8((y_end >> 8) & 0xFF);
    spi_write_data8(y_end & 0xFF);

    spi_write_cmd(0x2C);                   // RAMWR
}

// ---- Public HAL ----

void display_hal_init(void) {
    // Configure GPIO directions for SPI control lines.
    pinMode(LCD_CS, OUTPUT);
    pinMode(LCD_DC, OUTPUT);
    pinMode(LCD_RESET, OUTPUT);
    digitalWrite(LCD_CS, HIGH);

    // Backlight via LEDC PWM (ESP-Arduino 3.x API) — some panels have
    // an LDO/filter that needs PWM rather than a static DC level.
    ledcAttach(LCD_BL, BL_PWM_FREQ_HZ, BL_PWM_RES_BITS);
    ledcWrite(LCD_BL, 0);                  // off until init complete

    // Bring up SPI on the FSPI bus (ESP32-S3) with explicit pins.
    spi = new SPIClass(FSPI);
    spi->begin(LCD_SCLK, -1 /* miso */, LCD_MOSI, LCD_CS);

    // Hard reset the panel.
    digitalWrite(LCD_RESET, LOW);
    delay(20);
    digitalWrite(LCD_RESET, HIGH);
    delay(150);
}

void display_hal_begin(void) {
    send_init_sequence();
    display_hal_fill_screen(0x0000);
    ledcWrite(LCD_BL, 200);                // backlight ~80%
    Serial.println("NV3023 init complete");
}

void display_hal_set_brightness(uint8_t level) {
    ledcWrite(LCD_BL, level);
}

void display_hal_fill_screen(uint16_t color) {
    spi->beginTransaction(spi_settings);
    cs_lo();
    set_addr_window(0, 0, LCD_WIDTH, LCD_HEIGHT);
    uint8_t hi = (color >> 8) & 0xFF;
    uint8_t lo = color & 0xFF;
    uint8_t row_buf[284 * 2];
    for (int i = 0; i < LCD_WIDTH; i++) {
        row_buf[i*2]     = hi;
        row_buf[i*2 + 1] = lo;
    }
    for (int y = 0; y < LCD_HEIGHT; y++) {
        spi_write_data_bytes(row_buf, LCD_WIDTH * 2);
    }
    cs_hi();
    spi->endTransaction();
}

void display_hal_draw_bitmap(int32_t x, int32_t y, int32_t w, int32_t h,
                             const uint16_t* pixels) {
    // LVGL stores RGB565 in host-endian (little-endian on ESP32-S3).
    // NV3023 expects bytes MSB-first over SPI. Swap each pixel into a
    // row buffer before streaming so the panel sees the same byte
    // order that fillScreen produces.
    static uint16_t row_buf[284];
    spi->beginTransaction(spi_settings);
    cs_lo();
    set_addr_window(x, y, w, h);
    // Stream LVGL pixels verbatim — no byte swap, no channel swap.
    // Empirically determine the right transform from what shows up.
    (void)row_buf;
    spi_write_data_bytes((const uint8_t*)pixels, (size_t)w * (size_t)h * 2);
    cs_hi();
    spi->endTransaction();
}

void display_hal_tick(void) {}

lv_color_format_t display_hal_lv_color_format(void) {
    // NV3023 consumes bytes MSB-first over SPI. Ask LVGL to write
    // pixels in swapped (big-endian) byte order so we can stream the
    // buffer verbatim from draw_bitmap.
    return LV_COLOR_FORMAT_RGB565_SWAPPED;
}

void display_hal_round_area(int32_t* x1, int32_t* y1, int32_t* x2, int32_t* y2) {
    *x1 = *x1 & ~1;
    *y1 = *y1 & ~1;
    *x2 = *x2 | 1;
    *y2 = *y2 | 1;
}
