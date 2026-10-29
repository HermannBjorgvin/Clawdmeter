#include "../../hal/display_hal.h"
#include "board.h"
#include "io_expander.h"
#include "st7701_init.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>

// ST7701S over ESP32 RGB parallel, same scheme as the LCD-4 port: Arduino_GFX
// DMA-scans the PSRAM frame buffer, bounce_buffer_size_px gives ESP-IDF SRAM
// bounce buffers so CPU writes don't tear against the scan.
//
// The init SPI's chip select is expander EXIO3, not a GPIO, so the display
// gets no Arduino_GFX bus: display_hal_begin() pulls EXIO3 low and plays the
// init table through st7701_send_init() below, then starts the RGB panel.
// Timings are the Waveshare demo's (18 MHz PCLK, hsync 50/8/10, vsync 8/3/8).
//
// Do not call rgbpanel->getFrameBuffer() after gfx->begin() — that
// reconstructs the RGB panel a second time and crashes (no free slot).

static Arduino_ESP32RGBPanel* rgbpanel = nullptr;
static Arduino_RGB_Display*  gfx      = nullptr;

// 9-bit 3-wire SPI (D/C bit, then 8 bits MSB first; sampled on SCK rising),
// clocked at ~500 kHz. The demo runs this bus at 4 MHz. Arduino_SWSPI is not
// usable here: it toggles SCK at full register speed, and this panel ignores
// every command at that rate (black screen, backlight on).
static void spi9_write(bool data, uint8_t v) {
    uint16_t word = ((uint16_t)data << 8) | v;
    for (uint16_t bit = 0x100; bit; bit >>= 1) {
        digitalWrite(LCD_SPI_MOSI, (word & bit) ? HIGH : LOW);
        delayMicroseconds(1);
        digitalWrite(LCD_SPI_SCK, HIGH);
        delayMicroseconds(1);
        digitalWrite(LCD_SPI_SCK, LOW);
    }
}

// Plays an Arduino_GFX init-operations table (the subset st7701_init.h uses).
static void st7701_send_init(const uint8_t* ops, size_t len) {
    size_t i = 0;
    while (i < len) {
        switch (ops[i++]) {
        case BEGIN_WRITE:
        case END_WRITE:
            break;   // CS stays low for the whole table, as in the demo
        case WRITE_COMMAND_8:
            spi9_write(false, ops[i++]);
            break;
        case WRITE_BYTES: {
            uint8_t n = ops[i++];
            while (n--) spi9_write(true, ops[i++]);
            break;
        }
        case WRITE_C8_D8:
            spi9_write(false, ops[i++]);
            spi9_write(true, ops[i++]);
            break;
        case WRITE_C8_D16:
            spi9_write(false, ops[i++]);
            spi9_write(true, ops[i++]);
            spi9_write(true, ops[i++]);
            break;
        case DELAY:
            delay(ops[i++]);
            break;
        default:
            Serial.printf("st7701 init: unsupported op %u at %u\n", ops[i - 1], (unsigned)(i - 1));
            return;
        }
    }
}

void display_hal_init(void) {
    rgbpanel = new Arduino_ESP32RGBPanel(
        LCD_DE, LCD_VSYNC, LCD_HSYNC, LCD_PCLK,
        LCD_R0, LCD_R1, LCD_R2, LCD_R3, LCD_R4,
        LCD_G0, LCD_G1, LCD_G2, LCD_G3, LCD_G4, LCD_G5,
        LCD_B0, LCD_B1, LCD_B2, LCD_B3, LCD_B4,
        1 /* hsync_polarity */, 50 /* hsync_front_porch */,
        8 /* hsync_pulse_width */, 10 /* hsync_back_porch */,
        1 /* vsync_polarity */, 8 /* vsync_front_porch */,
        3 /* vsync_pulse_width */, 8 /* vsync_back_porch */,
        0 /* pclk_active_neg */, LCD_PCLK_HZ /* prefer_speed */,
        false /* useBigEndian */,
        0 /* de_idle_high */, 0 /* pclk_idle_high */,
        LCD_WIDTH * 10 /* bounce_buffer_size_px */);
    // No bus: begin() then skips its reset and init and only starts the
    // RGB panel. Reset is pulsed in board_init(), init is sent below.
    gfx = new Arduino_RGB_Display(
        LCD_WIDTH, LCD_HEIGHT, rgbpanel, 0 /* rotation */,
        true /* auto_flush */);
}

void display_hal_begin(void) {
    if (!gfx) return;
    pinMode(LCD_SPI_SCK, OUTPUT);
    pinMode(LCD_SPI_MOSI, OUTPUT);
    digitalWrite(LCD_SPI_SCK, LOW);
    io_expander_set(IOX_BIT_LCD_CS, false);
    st7701_send_init(st7701_lcd21_init_operations, sizeof(st7701_lcd21_init_operations));
    io_expander_set(IOX_BIT_LCD_CS, true);
    if (!gfx->begin()) Serial.println("LCD-2.1 RGB panel begin failed");
    gfx->fillScreen(0x0000);
    ledcAttach(LCD_BL, 5000 /* Hz */, 8 /* bits */);
    ledcWrite(LCD_BL, 200);
}

void display_hal_set_brightness(uint8_t level) {
    // ST7701S has no brightness command; the backlight is a PWM-driven FET.
    ledcWrite(LCD_BL, level);
}

void display_hal_draw_bitmap(int32_t x, int32_t y, int32_t w, int32_t h,
                             const uint16_t* pixels) {
    if (gfx) gfx->draw16bitRGBBitmap(x, y, (uint16_t*)pixels, w, h);
}

void display_hal_tick(void) {
    // No rotation cycle on this board.
}

void display_hal_round_area(int32_t* x1, int32_t* y1, int32_t* x2, int32_t* y2) {
    (void)x1; (void)y1; (void)x2; (void)y2;
}
