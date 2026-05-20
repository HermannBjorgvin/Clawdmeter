#include <Arduino.h>
#include <lvgl.h>
#include <ArduinoJson.h>
#include "freenove_board.h"
#include "display_freenove.h"
#include "data.h"
#include "ui_freenove.h"
#include "ble.h"

static UsageData usage = {};
static uint16_t* buf1 = nullptr;
static uint16_t* buf2 = nullptr;
#define BUF_LINES 32

static uint32_t my_tick(void) {
    return millis();
}

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

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("{\"ready\":true}");

    display_init();

    lv_init();
    lv_tick_set_cb(my_tick);

    buf1 = (uint16_t*)heap_caps_malloc(LCD_WIDTH * BUF_LINES * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    buf2 = (uint16_t*)heap_caps_malloc(LCD_WIDTH * BUF_LINES * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);

    lv_display_t* disp = lv_display_create(LCD_WIDTH, LCD_HEIGHT);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, display_flush);
    lv_display_set_buffers(disp, buf1, buf2, LCD_WIDTH * BUF_LINES * 2,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    ble_init();
    ui_init();
    ui_set_waiting_state();

    Serial.println("Minimal dashboard ready, waiting for BLE data...");
}

void loop() {
    lv_timer_handler();
    ble_tick();

    if (ble_has_data()) {
        if (parse_json(ble_get_data(), &usage)) {
            ui_update(&usage);
            ble_send_ack();
        } else {
            ble_send_nack();
        }
    }

    delay(5);
}
