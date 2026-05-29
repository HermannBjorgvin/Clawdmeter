// Minimum-viable red screen test.
// Init panel, fill entire FB with red ONCE, hold. No cycling, no rotation,
// no extras. If this doesn't show red, the panel write path is broken.

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_XCA9554.h>

#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
extern "C" {
#include "esp_lcd_axs15231b.h"
}

#define PIN_LCD_CS    9
#define PIN_LCD_SCLK  10
#define PIN_LCD_D0    11
#define PIN_LCD_D1    12
#define PIN_LCD_D2    13
#define PIN_LCD_D3    14
#define PIN_LCD_RST   21
#define PIN_LCD_BL    8
#define PIN_I2C_SDA   47
#define PIN_I2C_SCL   48
#define TCA9554_ADDR        0x20
#define TCA9554_BL_EN_PIN   1

#define LCD_W 172
#define LCD_H 640

static Adafruit_XCA9554 io_expander;
static esp_lcd_panel_io_handle_t panel_io = nullptr;
static esp_lcd_panel_handle_t    panel    = nullptr;

static uint32_t framed_cmd(uint8_t cmd) {
    return (0x02u << 24) | ((uint32_t)cmd << 8);
}

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println();
    Serial.println("[red] Clawdmeter 3.49 minimal red-screen test");

    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    Wire.setClock(400000);

    // TCA9554 → BL_EN HIGH
    if (!io_expander.begin(TCA9554_ADDR)) {
        Serial.println("[red] TCA9554 FAILED");
    } else {
        for (uint8_t p = 0; p <= 7; p++) io_expander.pinMode(p, OUTPUT);
        for (uint8_t p = 0; p <= 7; p++) io_expander.digitalWrite(p, LOW);
        delay(5);
        io_expander.digitalWrite(TCA9554_BL_EN_PIN, HIGH);
        Serial.println("[red] TCA9554 OK, BL_EN HIGH");
    }

    // Backlight diagnostic blink BEFORE touching SPI/panel — proves the BL
    // path works in isolation. If you see the screen pulse 3 times (bright/
    // dim/bright/dim/bright/dim), backlight is alive and any later "black"
    // is a pixel-write problem. If no pulse at all, BL is off and we need
    // to fix that first.
    pinMode(PIN_LCD_BL, OUTPUT);
    for (int i = 0; i < 3; i++) {
        digitalWrite(PIN_LCD_BL, LOW);   // bright
        Serial.println("[red] BL: bright");
        delay(500);
        digitalWrite(PIN_LCD_BL, HIGH);  // dim/off
        Serial.println("[red] BL: dim");
        delay(500);
    }
    digitalWrite(PIN_LCD_BL, LOW);  // settle bright
    Serial.println("[red] BL settled at max brightness");

    // SPI bus
    spi_bus_config_t bus_cfg = {};
    bus_cfg.sclk_io_num     = PIN_LCD_SCLK;
    bus_cfg.data0_io_num    = PIN_LCD_D0;
    bus_cfg.data1_io_num    = PIN_LCD_D1;
    bus_cfg.data2_io_num    = PIN_LCD_D2;
    bus_cfg.data3_io_num    = PIN_LCD_D3;
    bus_cfg.max_transfer_sz = LCD_W * LCD_H * 2;
    bus_cfg.flags           = SPICOMMON_BUSFLAG_QUAD;
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    Serial.printf("[red] spi_bus_initialize err=%d\n", err);

    // Panel IO (QSPI). Lower PCLK to 20 MHz in case 40 MHz was unstable.
    esp_lcd_panel_io_spi_config_t io_cfg = {};
    io_cfg.cs_gpio_num   = PIN_LCD_CS;
    io_cfg.dc_gpio_num   = -1;
    io_cfg.spi_mode      = 3;       // AXS15231B requires SPI mode 3 per driver macro
    io_cfg.pclk_hz       = 20 * 1000 * 1000;
    io_cfg.trans_queue_depth = 10;
    io_cfg.lcd_cmd_bits   = 32;
    io_cfg.lcd_param_bits = 8;
    io_cfg.flags.quad_mode = true;
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &panel_io);
    Serial.printf("[red] new_panel_io_spi err=%d\n", err);

    // Panel
    axs15231b_vendor_config_t vendor_cfg = {};
    vendor_cfg.flags.use_qspi_interface = 1;
    esp_lcd_panel_dev_config_t panel_cfg = {};
    panel_cfg.reset_gpio_num = PIN_LCD_RST;
    panel_cfg.rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_cfg.bits_per_pixel = 16;
    panel_cfg.vendor_config  = &vendor_cfg;
    err = esp_lcd_new_panel_axs15231b(panel_io, &panel_cfg, &panel);
    Serial.printf("[red] new_panel_axs15231b err=%d\n", err);

    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);

    // Try BOTH invert states by toggling — if invert=true left it black,
    // maybe invert=false is right for this panel.
    esp_lcd_panel_invert_color(panel, false);
    esp_lcd_panel_disp_on_off(panel, true);

    // Wake from "All Pixels Off" mode that the vendor init sequence puts the
    // panel into. Properly-framed for QSPI.
    esp_lcd_panel_io_tx_param(panel_io, framed_cmd(0x13), NULL, 0);  // NORON
    esp_lcd_panel_io_tx_param(panel_io, framed_cmd(0x29), NULL, 0);  // DISPON
    delay(20);
    Serial.println("[red] init complete, about to fill red");

    // Allocate one full-screen RGB565 buffer in internal DMA RAM.
    // 172*640*2 = 220,160 bytes — won't fit in DRAM if other code is running,
    // but this minimal sketch leaves plenty headroom.
    size_t fb_bytes = LCD_W * LCD_H * 2;
    Serial.printf("[red] free internal DRAM before alloc: %u\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    uint16_t *fb = (uint16_t*)heap_caps_malloc(fb_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!fb) {
        Serial.println("[red] DMA alloc FAILED, falling back to strip mode");
        // Fall back to 40-row strips
        const int STRIP_H = 40;
        uint16_t *strip = (uint16_t*)heap_caps_malloc(LCD_W * STRIP_H * 2,
                                                      MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!strip) { Serial.println("[red] strip alloc also FAILED"); return; }
        // RED 0xF800, byte-swapped for big-endian wire format
        uint16_t v = 0x00F8;  // 0xF800 swapped
        for (int i = 0; i < LCD_W * STRIP_H; i++) strip[i] = v;
        for (int y = 0; y < LCD_H; y += STRIP_H) {
            int h = (y + STRIP_H <= LCD_H) ? STRIP_H : (LCD_H - y);
            esp_lcd_panel_draw_bitmap(panel, 0, y, LCD_W, y + h, strip);
        }
        Serial.println("[red] strip-mode red fill complete");
        return;
    }
    // Whole-screen red.
    uint16_t v = 0x00F8;  // 0xF800 byte-swapped
    for (int i = 0; i < LCD_W * LCD_H; i++) fb[i] = v;
    err = esp_lcd_panel_draw_bitmap(panel, 0, 0, LCD_W, LCD_H, fb);
    Serial.printf("[red] draw_bitmap err=%d — should be solid red now\n", err);
}

void loop() {
    static uint32_t last = 0;
    if (millis() - last > 2000) {
        last = millis();
        Serial.println("[red] alive (panel should still be red)");
    }
}
