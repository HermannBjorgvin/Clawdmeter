#include "../../hal/display_hal.h"
#include "board.h"
#include "display_color_order.h"

#include <Adafruit_ST7789.h>
#include <Arduino.h>
#include <SPI.h>

static SPIClass display_spi(HSPI);
static Adafruit_ST7789 display(
    &display_spi,
    LCD_CS,
    LCD_DC,
    LCD_RESET
);

static int32_t clamp_coordinate(int32_t value, int32_t maximum) {
    if (value < 0) {
        return 0;
    }
    if (value > maximum) {
        return maximum;
    }
    return value;
}

void display_hal_init(void) {
    display_spi.begin(LCD_SCLK, LCD_MISO, LCD_MOSI, LCD_CS);
    if (!ledcAttach(LCD_BACKLIGHT, 5000, 8)) {
        Serial.println("Backlight PWM attach failed");
    }
}

void display_hal_begin(void) {
    display.init(LCD_NATIVE_WIDTH, LCD_NATIVE_HEIGHT, SPI_MODE0);
    display.setSPISpeed(40000000);
    display.setRotation(LCD_ROTATION);
    uint8_t madctl = st7789_bgr_madctl(LCD_ROTATION);
    display.sendCommand(ST77XX_MADCTL, &madctl, 1);
    display.invertDisplay(false);
    display.fillScreen(ST77XX_BLACK);
    display_hal_set_brightness(200);
    Serial.printf(
        "Display ST7789 ready (%dx%d %s, HSPI 40MHz)\n",
        LCD_WIDTH,
        LCD_HEIGHT,
#ifdef BOARD_LANDSCAPE
        "landscape"
#else
        "portrait"
#endif
    );
}

void display_hal_set_brightness(uint8_t level) {
    ledcWrite(LCD_BACKLIGHT, level);
}

void display_hal_fill_screen(uint16_t color) {
    display.fillScreen(color);
}

void display_hal_draw_bitmap(
    int32_t x,
    int32_t y,
    int32_t width,
    int32_t height,
    const uint16_t* pixels
) {
    display.drawRGBBitmap(x, y, pixels, width, height);
}

void display_hal_tick(void) {}

void display_hal_round_area(
    int32_t* x1,
    int32_t* y1,
    int32_t* x2,
    int32_t* y2
) {
    *x1 = clamp_coordinate(*x1, LCD_WIDTH - 1);
    *y1 = clamp_coordinate(*y1, LCD_HEIGHT - 1);
    *x2 = clamp_coordinate(*x2, LCD_WIDTH - 1);
    *y2 = clamp_coordinate(*y2, LCD_HEIGHT - 1);
}
