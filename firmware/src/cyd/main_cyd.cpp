#include <Arduino.h>
#include <lvgl.h>
#include <SPI.h>
#include <Arduino_GFX_Library.h>
#include <XPT2046_Bitbang.h>
#include <ArduinoJson.h>
#include "../data.h"
#include "../ble.h"
#include "../ui.h"
#include "../splash.h"
#include "../usage_rate.h"

// ---- ILI9341 TFT (VSPI bus) ----
#define TFT_CS    15
#define TFT_DC    2
#define TFT_SCK   14
#define TFT_MOSI  13
#define TFT_MISO  12
#define TFT_BL    21
#define TFT_RST   -1   // tied to ESP32 EN

// ---- XPT2046 resistive touch (HSPI bus, separate from TFT) ----
#define TS_CS     33
#define TS_IRQ    36
#define TS_SCK    25
#define TS_MOSI   32
#define TS_MISO   39

// ---- Display geometry (landscape) ----
#define LCD_W 320
#define LCD_H 240

// Per-unit raw-ADC calibration for XPT2046. CYD panels vary; tweak if the
// pointer is offset or compressed at the edges. These defaults come from
// the most common ESP32-2432S028R production run.
#define TS_RAW_X_MIN 340
#define TS_RAW_X_MAX 3900
#define TS_RAW_Y_MIN 200
#define TS_RAW_Y_MAX 3850

Arduino_DataBus *bus = new Arduino_ESP32SPI(TFT_DC, TFT_CS, TFT_SCK, TFT_MOSI, TFT_MISO, VSPI);
// CYD panel is IPS — needs INVON to render colors correctly (otherwise the
// display looks photo-negative: black bg → white, etc.).
// Rotation 3 = landscape flipped 180° from rotation 1. Pick whichever matches
// your enclosure orientation; touch mapping below flips with it.
Arduino_GFX     *gfx = new Arduino_ILI9341(bus, TFT_RST, 3 /* 270° landscape flipped */, true /* IPS */);

// Bitbang touch — keeps off the hardware SPI buses entirely.
static XPT2046_Bitbang ts(TS_MOSI, TS_MISO, TS_SCK, TS_CS);

static UsageData usage = {};

// Allocated at runtime so they don't count against the DRAM .bss budget
// (NimBLE eats a lot of fixed RAM; heap has more headroom than .bss).
// Smaller buf saves heap room for the 80KB splash canvas.
#define BUF_LINES 20
static uint16_t *buf1 = nullptr;
static uint16_t *buf2 = nullptr;

static uint32_t my_tick(void) { return millis(); }

static void my_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px) {
    int32_t w = area->x2 - area->x1 + 1;
    int32_t h = area->y2 - area->y1 + 1;
    gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)px, w, h);
    lv_display_flush_ready(disp);
}

static void my_touch_cb(lv_indev_t *indev, lv_indev_data_t *data) {
    // PEN IRQ goes LOW only when the panel is pressed. Reading it cheaply
    // gates the (slower) bitbang ADC read so we don't pound SPI every frame.
    if (digitalRead(TS_IRQ) == HIGH) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    TouchPoint p = ts.getTouch();
    // zRaw is pressure; very low values are spurious from contact bounce.
    if (p.zRaw < 200) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    // Landscape rotation 3 (180° flipped from rotation 1): both axes inverted
    // relative to the rotation-1 mapping.
    int16_t x = map(p.yRaw, TS_RAW_Y_MAX, TS_RAW_Y_MIN, 0, LCD_W);
    int16_t y = map(p.xRaw, TS_RAW_X_MIN, TS_RAW_X_MAX, 0, LCD_H);
    if (x < 0) x = 0; else if (x >= LCD_W) x = LCD_W - 1;
    if (y < 0) y = 0; else if (y >= LCD_H) y = LCD_H - 1;
    data->point.x = x;
    data->point.y = y;
    data->state = LV_INDEV_STATE_PRESSED;
}

static bool parse_json(const char *json, UsageData *out) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        Serial.printf("JSON parse error: %s\n", err.c_str());
        return false;
    }
    out->session_pct        = doc["s"]  | 0.0f;
    out->session_reset_mins = doc["sr"] | -1;
    out->weekly_pct         = doc["w"]  | 0.0f;
    out->weekly_reset_mins  = doc["wr"] | -1;
    strlcpy(out->status, doc["st"] | "unknown", sizeof(out->status));
    out->ok    = doc["ok"] | false;
    out->valid = true;

    out->codex.session_pct        = doc["cx_s"]  | 0.0f;
    out->codex.session_reset_mins = doc["cx_sr"] | -1;
    out->codex.weekly_pct         = doc["cx_w"]  | 0.0f;
    out->codex.weekly_reset_mins  = doc["cx_wr"] | -1;
    out->codex.valid = doc.containsKey("cx_s");
    return true;
}

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("{\"ready\":true,\"board\":\"cyd\"}");

    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);

    gfx->begin();
    gfx->fillScreen(0x0000);

    pinMode(TS_IRQ, INPUT);
    ts.begin();

    lv_init();
    lv_tick_set_cb(my_tick);

    const size_t buf_bytes = LCD_W * BUF_LINES * sizeof(uint16_t);
    buf1 = (uint16_t *)heap_caps_malloc(buf_bytes, MALLOC_CAP_8BIT);
    buf2 = (uint16_t *)heap_caps_malloc(buf_bytes, MALLOC_CAP_8BIT);
    if (!buf1 || !buf2) {
        Serial.printf("FATAL: LVGL buffer alloc failed (need %u bytes each)\n",
                      (unsigned)buf_bytes);
        while (true) delay(1000);
    }

    lv_display_t *disp = lv_display_create(LCD_W, LCD_H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, my_flush_cb);
    lv_display_set_buffers(disp, buf1, buf2, buf_bytes,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, my_touch_cb);

    ble_init();
    Serial.printf("free heap before UI init: %u (largest contig: %u)\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    ui_init();
    Serial.printf("free heap after UI init:  %u\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    ui_update_ble_status(ble_get_state(), ble_get_device_name(), ble_get_mac_address());
    ui_show_screen(SCREEN_SPLASH);

    Serial.println("CYD dashboard ready, waiting for BLE data...");
}

static ble_state_t last_ble_state = BLE_STATE_INIT;

void loop() {
    lv_timer_handler();
    ui_tick_anim();
    ble_tick();
    splash_tick();

    ble_state_t bs = ble_get_state();
    if (bs != last_ble_state) {
        last_ble_state = bs;
        ui_update_ble_status(bs, ble_get_device_name(), ble_get_mac_address());
    }

    if (ble_has_data()) {
        if (parse_json(ble_get_data(), &usage)) {
            // Feed rate sampler so the splash picks heavier animations as
            // session % climbs faster.
            int g_before = usage_rate_group();
            usage_rate_sample(usage.session_pct);
            int g_after = usage_rate_group();
            if (g_after != g_before && splash_is_active()) {
                splash_pick_for_current_rate();
            }
            ui_update(&usage);
            ui_update_codex(&usage.codex);
            ble_send_ack();
        } else {
            ble_send_nack();
        }
    }
    delay(5);
}
