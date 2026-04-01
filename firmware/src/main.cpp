#include <Arduino.h>
#include <lvgl.h>
#include <ArduinoJson.h>
#include "display_cfg.h"
#include "data.h"
#include "ui.h"

static LGFX lcd;
static UsageData usage = {};

// LVGL draw buffers
#define BUF_LINES 40
static uint16_t buf1[480 * BUF_LINES];
static uint16_t buf2[480 * BUF_LINES];

// Serial line buffer
#define SERIAL_BUF_SIZE 512
static char serial_buf[SERIAL_BUF_SIZE];
static int serial_pos = 0;

// LVGL tick callback
static uint32_t my_tick(void) {
    return millis();
}

// LVGL flush callback
static void my_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);
    lcd.startWrite();
    lcd.setAddrWindow(area->x1, area->y1, w, h);
    lcd.writePixels((uint16_t*)px_map, w * h, true);
    lcd.endWrite();
    lv_display_flush_ready(disp);
}

// LVGL touch callback
static void my_touch_cb(lv_indev_t* indev, lv_indev_data_t* data) {
    uint16_t x, y;
    if (lcd.getTouch(&x, &y)) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

// Parse a JSON line into UsageData
static bool parse_json(const char* json, UsageData* out) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        Serial.printf("JSON parse error: %s\n", err.c_str());
        return false;
    }

    out->session_pct = doc["s"] | 0.0f;
    out->session_reset_mins = doc["sr"] | -1;
    out->weekly_pct = doc["w"] | 0.0f;
    out->weekly_reset_mins = doc["wr"] | -1;
    strlcpy(out->status, doc["st"] | "unknown", sizeof(out->status));
    out->ok = doc["ok"] | false;
    out->valid = true;
    return true;
}

// Read serial data, returns true when a complete line is ready
static bool read_serial_line() {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n') {
            serial_buf[serial_pos] = '\0';
            serial_pos = 0;
            return true;
        }
        if (serial_pos < SERIAL_BUF_SIZE - 1) {
            serial_buf[serial_pos++] = c;
        }
    }
    return false;
}

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("{\"ready\":true}");

    // Init display
    lcd.init();
    lcd.setRotation(1);
    lcd.setBrightness(200);
    lcd.fillScreen(TFT_BLACK);

    // Init LVGL
    lv_init();
    lv_tick_set_cb(my_tick);

    lv_display_t* disp = lv_display_create(480, 320);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, my_flush_cb);
    lv_display_set_buffers(disp, buf1, buf2, sizeof(buf1), LV_DISPLAY_RENDER_MODE_PARTIAL);

    lv_indev_t* indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, my_touch_cb);

    // Build dashboard
    ui_init();

    Serial.println("Dashboard ready, waiting for data on serial...");
}

void loop() {
    lv_timer_handler();
    ui_tick_anim();

    if (read_serial_line()) {
        if (parse_json(serial_buf, &usage)) {
            ui_update(&usage);
            Serial.println("{\"ack\":true}");
        }
    }

    delay(5);
}
