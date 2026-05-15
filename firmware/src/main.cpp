#include <Arduino.h>
#include <lvgl.h>
#include <ArduinoJson.h>
#include "display_cfg.h"
#include "data.h"
#include "ui.h"
#include "ble.h"
#include "power.h"
#include "imu.h"
#include "splash.h"
#include "usage_rate.h"
#include "buttons.h"

// ============================================================
// Board-specific display driver instantiation
// ============================================================
//
// Each board wires up `gfx` (Arduino_GFX base pointer) and provides a
// `board_set_brightness(level)` helper used during the rotation flash.
// Everything below this section is board-agnostic.

#if defined(BOARD_WAVESHARE_AMOLED_216)

static Arduino_DataBus  *bus      = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
static Arduino_CO5300   *amoled   = new Arduino_CO5300(
    bus, LCD_RESET, 0 /* rotation; we rotate in software */,
    BOARD_LCD_W, BOARD_LCD_H, 0, 0, 0, 0);
Arduino_GFX *gfx = amoled;

void board_set_brightness(uint8_t level) { amoled->setBrightness(level); }

TouchDrvCST92xx touch;
XPowersPMU      pmu;
SensorQMI8658   imu;

#elif defined(BOARD_LILYGO_T_DISPLAY_S3)

// 8-bit i80 parallel bus + ST7789. The ST7789 panel internally is 240x320
// but only 170 columns are wired out, so the column window starts at
// x=LCD_COL_OFFSET.
static Arduino_DataBus *bus = new Arduino_ESP32LCD8(
    LCD_DC, LCD_CS, LCD_WR, LCD_RD,
    LCD_D0, LCD_D1, LCD_D2, LCD_D3,
    LCD_D4, LCD_D5, LCD_D6, LCD_D7);
static Arduino_ST7789 *st7789 = new Arduino_ST7789(
    bus, LCD_RESET, BOARD_FIXED_ROTATION,
    true /* IPS */, BOARD_LCD_W, BOARD_LCD_H,
    LCD_COL_OFFSET, LCD_ROW_OFFSET, LCD_COL_OFFSET, LCD_ROW_OFFSET);
Arduino_GFX *gfx = st7789;

// Backlight is a plain GPIO on this board. Map [0..0] to OFF, anything
// else to ON. A future change could put the pin on LEDC for true PWM.
void board_set_brightness(uint8_t level) {
    digitalWrite(LCD_BL, level > 0 ? HIGH : LOW);
}

#endif

// ============================================================
// LVGL plumbing
// ============================================================

static UsageData usage = {};

// ---- Touch (Waveshare only) ----
#if BOARD_HAS_TOUCH
static volatile bool     touch_pressed = false;
static volatile uint16_t touch_x = 0;
static volatile uint16_t touch_y = 0;
static volatile bool     touch_data_ready = false;

static void IRAM_ATTR touch_isr(void) {
    touch_data_ready = true;
}

static void touch_read(void) {
    if (!touch_data_ready) return;
    touch_data_ready = false;

    int16_t tx[5], ty[5];
    uint8_t n = touch.getPoint(tx, ty, touch.getSupportTouchPoint());
    if (n > 0) {
        touch_pressed = true;
        touch_x = (uint16_t)tx[0];
        touch_y = (uint16_t)ty[0];
    } else {
        touch_pressed = false;
    }
}

static void my_touch_cb(lv_indev_t* indev, lv_indev_data_t* data) {
    if (touch_pressed) {
        data->point.x = touch_x;
        data->point.y = touch_y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}
#endif // BOARD_HAS_TOUCH

// ---- LVGL draw buffers (PSRAM-backed, partial render) ----
// Use the larger dimension so the buffer also holds rotated strips: on
// the Waveshare we rotate strips in software, so a BOARD_LCD_W-wide strip
// could end up BOARD_LCD_W tall after a 90° rotation.
#define BUF_LINES 40
#define BUF_STRIDE  ((BOARD_LCD_W > BOARD_LCD_H) ? BOARD_LCD_W : BOARD_LCD_H)
static uint16_t *buf1 = nullptr;
static uint16_t *buf2 = nullptr;
#if BOARD_HAS_IMU
static uint16_t *rot_buf = nullptr;   // only the software-rotation path needs this
#endif

static uint32_t my_tick(void) { return millis(); }

#if BOARD_HAS_IMU
// CO5300 cannot exchange rows/columns in hardware — its MADCTL only does
// axis flips — so when the IMU reports a 90°/180°/270° orientation we
// rotate each LVGL strip in software before pushing pixels.
static void rotate_strip(const uint16_t *src, int32_t w, int32_t h,
                         int32_t sx, int32_t sy, uint8_t r,
                         int32_t *dx, int32_t *dy, int32_t *dw, int32_t *dh) {
    const int S = BOARD_LCD_W;   // square panel: W == H

    switch (r) {
    case 1: // 90° CW
        *dw = h; *dh = w;
        *dx = S - sy - h;
        *dy = sx;
        for (int32_t y = 0; y < h; y++)
            for (int32_t x = 0; x < w; x++)
                rot_buf[x * h + (h - 1 - y)] = src[y * w + x];
        break;
    case 2: // 180°
        *dw = w; *dh = h;
        *dx = S - sx - w;
        *dy = S - sy - h;
        for (int32_t y = 0; y < h; y++)
            for (int32_t x = 0; x < w; x++)
                rot_buf[(h - 1 - y) * w + (w - 1 - x)] = src[y * w + x];
        break;
    case 3: // 270° CW
        *dw = h; *dh = w;
        *dx = sy;
        *dy = S - sx - w;
        for (int32_t y = 0; y < h; y++)
            for (int32_t x = 0; x < w; x++)
                rot_buf[(w - 1 - x) * h + y] = src[y * w + x];
        break;
    default:
        *dx = sx; *dy = sy; *dw = w; *dh = h;
        break;
    }
}
#endif // BOARD_HAS_IMU

static void my_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    int32_t w = area->x2 - area->x1 + 1;
    int32_t h = area->y2 - area->y1 + 1;
    uint16_t *src = (uint16_t*)px_map;

#if BOARD_HAS_IMU
    uint8_t r = imu_get_rotation();
    if (r != 0) {
        int32_t dx, dy, dw, dh;
        rotate_strip(src, w, h, area->x1, area->y1, r, &dx, &dy, &dw, &dh);
        gfx->draw16bitRGBBitmap(dx, dy, rot_buf, dw, dh);
        lv_display_flush_ready(disp);
        return;
    }
#endif
    gfx->draw16bitRGBBitmap(area->x1, area->y1, src, w, h);
    lv_display_flush_ready(disp);
}

#if BOARD_LCD_IS_AMOLED
// CO5300 requires even-aligned flush regions; ST7789 doesn't care.
static void rounder_cb(lv_event_t* e) {
    lv_area_t *area = (lv_area_t*)lv_event_get_param(e);
    area->x1 = area->x1 & ~1;
    area->y1 = area->y1 & ~1;
    area->x2 = area->x2 | 1;
    area->y2 = area->y2 | 1;
}
#endif

// ============================================================
// JSON / serial command handling
// ============================================================

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

#define CMD_BUF_SIZE 64
static char cmd_buf[CMD_BUF_SIZE];
static int cmd_pos = 0;

static void send_screenshot(void) {
    const uint32_t w = BOARD_LCD_W, h = BOARD_LCD_H;
    const uint32_t row_bytes = w * 2;
    const uint32_t buf_size = row_bytes * h;
    uint8_t* sbuf = (uint8_t*)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!sbuf) {
        Serial.println("SCREENSHOT_ERR");
        return;
    }

    lv_draw_buf_t draw_buf;
    lv_draw_buf_init(&draw_buf, w, h, LV_COLOR_FORMAT_RGB565, row_bytes, sbuf, buf_size);

    lv_result_t res = lv_snapshot_take_to_draw_buf(lv_screen_active(), LV_COLOR_FORMAT_RGB565, &draw_buf);
    if (res != LV_RESULT_OK) {
        heap_caps_free(sbuf);
        Serial.println("SCREENSHOT_ERR");
        return;
    }

    Serial.printf("SCREENSHOT_START %lu %lu %lu\n", (unsigned long)w, (unsigned long)h, (unsigned long)buf_size);
    Serial.flush();
    Serial.write(sbuf, buf_size);
    Serial.flush();
    Serial.println();
    Serial.println("SCREENSHOT_END");

    heap_caps_free(sbuf);
}

static void check_serial_cmd(void) {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            cmd_buf[cmd_pos] = '\0';
            if (strcmp(cmd_buf, "screenshot") == 0) send_screenshot();
            cmd_pos = 0;
        } else if (cmd_pos < CMD_BUF_SIZE - 1) {
            cmd_buf[cmd_pos++] = c;
        }
    }
}

// ============================================================
// setup / loop
// ============================================================

void setup(void) {
    Serial.begin(115200);
    delay(300);
    Serial.println("{\"ready\":true}");
    Serial.printf("Board: %s\n", BOARD_NAME);
    Serial.printf("Display: %dx%d %s\n", BOARD_LCD_W, BOARD_LCD_H,
                  BOARD_LCD_IS_AMOLED ? "AMOLED (CO5300)" : "LCD (ST7789)");

#if defined(BOARD_LILYGO_T_DISPLAY_S3)
    // Bring the panel power rail up first so the ST7789 has VCC when we
    // talk to it. Then drive the backlight high so the user sees pixels.
    pinMode(LCD_POWER_ON, OUTPUT);
    digitalWrite(LCD_POWER_ON, HIGH);
    pinMode(LCD_BL, OUTPUT);
    digitalWrite(LCD_BL, HIGH);
    pinMode(LCD_RD, OUTPUT);
    digitalWrite(LCD_RD, HIGH);
#endif

#if BOARD_HAS_I2C
    Wire.begin(IIC_SDA, IIC_SCL);
#endif

    gfx->begin();
    gfx->fillScreen(0x0000);
    board_set_brightness(200);
    Serial.println("Display init OK");

    power_init();
    imu_init();

#if BOARD_HAS_TOUCH
    touch.setPins(TP_RST, TP_INT);
    if (!touch.begin(Wire, CST9220_ADDR, IIC_SDA, IIC_SCL)) {
        Serial.println("Touch init failed");
    } else {
        touch.setMaxCoordinates(BOARD_LCD_W, BOARD_LCD_H);
        touch.setSwapXY(true);
        touch.setMirrorXY(true, false);
        attachInterrupt(TP_INT, touch_isr, FALLING);
        Serial.println("Touch init OK");
    }
#else
    Serial.println("Touch: not present on this board");
#endif

    lv_init();
    lv_tick_set_cb(my_tick);

    // PSRAM-backed partial render buffers. Sized to the wider edge so
    // rotated strips fit too (Waveshare 480×40 rotates to 40×480).
    const size_t buf_pixels = BUF_STRIDE * BUF_LINES;
    buf1 = (uint16_t*)heap_caps_malloc(buf_pixels * 2, MALLOC_CAP_SPIRAM);
    buf2 = (uint16_t*)heap_caps_malloc(buf_pixels * 2, MALLOC_CAP_SPIRAM);
#if BOARD_HAS_IMU
    rot_buf = (uint16_t*)heap_caps_malloc(buf_pixels * 2, MALLOC_CAP_SPIRAM);
#endif

    lv_display_t* disp = lv_display_create(BOARD_LCD_W, BOARD_LCD_H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, my_flush_cb);
    lv_display_set_buffers(disp, buf1, buf2, buf_pixels * 2,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

#if BOARD_LCD_IS_AMOLED
    lv_display_add_event_cb(disp, rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);
#endif

#if BOARD_HAS_TOUCH
    lv_indev_t* indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, my_touch_cb);
#endif

    ble_init();
    buttons_init();
    Serial.println("Buttons init OK");

    ui_init();
    ui_update_ble_status(ble_get_state(), ble_get_device_name(), ble_get_mac_address());
    ui_update_battery(power_battery_pct(), power_is_charging());

    ui_show_screen(SCREEN_SPLASH);

    Serial.println("Dashboard ready, waiting for data on BLE...");
}

static ble_state_t last_ble_state = BLE_STATE_INIT;

#if BOARD_HAS_IMU
// On rotation change the panel is blanked, LVGL is forced to redraw at
// the new orientation, then the brightness is ramped back up over ~125ms
// so the transition reads as deliberate instead of a glitch.
static void handle_rotation_change(void) {
    static uint8_t last_rotation = 0;
    static uint8_t  ramp_step = 0;   // 0=idle, 1-4=ramping
    static uint32_t ramp_last = 0;

    uint8_t rot = imu_get_rotation();
    if (rot != last_rotation) {
        board_set_brightness(0);
        last_rotation = rot;
        lv_obj_invalidate(lv_screen_active());
        ramp_step = 1;
        return;
    }

    if (ramp_step == 0) return;
    uint32_t now = millis();
    if (now - ramp_last < 25) return;
    ramp_last = now;

    static const uint8_t levels[] = {60, 120, 170, 200};
    board_set_brightness(levels[ramp_step - 1]);
    if (ramp_step >= 4) ramp_step = 0;
    else                ramp_step++;
}
#endif

void loop(void) {
#if BOARD_HAS_TOUCH
    touch_read();
#endif
    lv_timer_handler();
    ui_tick_anim();
    ble_tick();
    power_tick();
    imu_tick();
    splash_tick();
    buttons_tick();

#if BOARD_HAS_IMU
    handle_rotation_change();
#endif

    ble_state_t bs = ble_get_state();
    if (bs != last_ble_state) {
        last_ble_state = bs;
        ui_update_ble_status(bs, ble_get_device_name(), ble_get_mac_address());
    }

    static int last_pct = -2;
    static bool last_charging = false;
    int pct = power_battery_pct();
    bool charging = power_is_charging();
    if (pct != last_pct || charging != last_charging) {
        last_pct = pct;
        last_charging = charging;
        ui_update_battery(pct, charging);
    }

    check_serial_cmd();

    if (ble_has_data()) {
        if (parse_json(ble_get_data(), &usage)) {
            int g_before = usage_rate_group();
            usage_rate_sample(usage.session_pct);
            int g_after = usage_rate_group();
            if (g_after != g_before) {
                Serial.printf("usage rate: group %d -> %d (s=%.2f%%)\n",
                              g_before, g_after, usage.session_pct);
                if (splash_is_active()) splash_pick_for_current_rate();
            }
            ui_update(&usage);
            ble_send_ack();
        } else {
            ble_send_nack();
        }
    }

    delay(5);
}
