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

#ifdef BOARD_XINGZHI_CUBE
// ---- xingzhi-cube hardware objects ----
// Buttons: BTN_WAKE/BTN_VOL/BTN_MID defined in display_cfg.h
// Unified names for shared button logic:
#define BTN_LEFT BTN_WAKE // GPIO  0 — Space / voice push-to-talk
#define BTN_RIGHT BTN_VOL // GPIO 40 — Shift+Tab / mode toggle
// BTN_MID (GPIO 39) replaces AXP PWR button for screen cycling

// Vendor-specific init sequence for the xingzhi-cube 1.83" ST7789V-like panel.
// Recovered from the board's ESPHome YAML. Without this the screen stays blank.
// Includes SLPOUT at the end, followed by the minimal standard setup.
// NOTE: Arduino_ST7789::tftInit() always sends SWRESET which would wipe these
// vendor registers, so we do NOT call super — we handle the full init here.
static const uint8_t xingzhi_vendor_init[] = {
    BEGIN_WRITE,
    WRITE_C8_BYTES,
    0xFD,
    2,
    0x06,
    0x08,
    WRITE_C8_BYTES,
    0x61,
    2,
    0x07,
    0x04,
    WRITE_C8_BYTES,
    0x62,
    3,
    0x00,
    0x44,
    0x45,
    WRITE_C8_BYTES,
    0x63,
    4,
    0x41,
    0x07,
    0x12,
    0x12,
    WRITE_C8_D8,
    0x64,
    0x37,
    WRITE_C8_BYTES,
    0x65,
    3,
    0x09,
    0x10,
    0x21,
    WRITE_C8_BYTES,
    0x66,
    3,
    0x09,
    0x10,
    0x21,
    WRITE_C8_BYTES,
    0x67,
    2,
    0x20,
    0x40,
    WRITE_C8_BYTES,
    0x68,
    4,
    0x90,
    0x4C,
    0x7C,
    0x66,
    WRITE_C8_BYTES,
    0xB1,
    3,
    0x0F,
    0x02,
    0x01,
    WRITE_C8_D8,
    0xB4,
    0x01,
    WRITE_C8_BYTES,
    0xB5,
    4,
    0x02,
    0x02,
    0x0A,
    0x14,
    WRITE_C8_BYTES,
    0xB6,
    5,
    0x04,
    0x01,
    0x9F,
    0x00,
    0x02,
    WRITE_C8_D8,
    0xDF,
    0x11,
    WRITE_C8_BYTES,
    0xE2,
    6,
    0x13,
    0x00,
    0x00,
    0x30,
    0x33,
    0x3F,
    WRITE_C8_BYTES,
    0xE5,
    6,
    0x3F,
    0x33,
    0x30,
    0x00,
    0x00,
    0x13,
    WRITE_C8_BYTES,
    0xE1,
    2,
    0x00,
    0x57,
    WRITE_C8_BYTES,
    0xE4,
    2,
    0x58,
    0x00,
    WRITE_C8_BYTES,
    0xE0,
    8,
    0x01,
    0x03,
    0x0D,
    0x0E,
    0x0E,
    0x0C,
    0x15,
    0x19,
    WRITE_C8_BYTES,
    0xE3,
    8,
    0x1A,
    0x16,
    0x0C,
    0x0F,
    0x0E,
    0x0D,
    0x02,
    0x01,
    WRITE_C8_BYTES,
    0xE6,
    2,
    0x00,
    0xFF,
    WRITE_C8_BYTES,
    0xE7,
    6,
    0x01,
    0x04,
    0x03,
    0x03,
    0x00,
    0x12,
    WRITE_C8_BYTES,
    0xE8,
    3,
    0x00,
    0x70,
    0x00,
    WRITE_C8_D8,
    0xEC,
    0x52,
    WRITE_C8_BYTES,
    0xF1,
    3,
    0x01,
    0x01,
    0x02,
    WRITE_C8_BYTES,
    0xF6,
    4,
    0x09,
    0x10,
    0x00,
    0x00,
    WRITE_C8_BYTES,
    0xFD,
    2,
    0xFA,
    0xFC,
    WRITE_C8_D8,
    0x35,
    0x00, // TEON  — tearing effect line on
    WRITE_COMMAND_8,
    0x11, // SLPOUT
    DELAY,
    120, // 120 ms for SLPOUT to settle
    WRITE_C8_D8,
    0x3A,
    0x55, // COLMOD — RGB565
    WRITE_COMMAND_8,
    0x13, // NORON
    WRITE_COMMAND_8,
    0x29, // DISPON
    END_WRITE,
};

class CalibratedST7789 : public Arduino_ST7789
{
public:
    using Arduino_ST7789::Arduino_ST7789;

    void setPanelOffsets(uint8_t c1, uint8_t r1, uint8_t c2, uint8_t r2, uint8_t rot)
    {
        COL_OFFSET1 = c1;
        ROW_OFFSET1 = r1;
        COL_OFFSET2 = c2;
        ROW_OFFSET2 = r2;
        setRotation(rot);
    }

protected:
    void tftInit() override
    {
        // Hardware reset — Arduino_ST7789::tftInit() would SWRESET after this,
        // wiping our vendor registers, so we handle the full init ourselves.
        if (_rst != GFX_NOT_DEFINED)
        {
            pinMode(_rst, OUTPUT);
            digitalWrite(_rst, HIGH);
            delay(10);
            digitalWrite(_rst, LOW);
            delay(20);
            digitalWrite(_rst, HIGH);
            delay(120);
        }
        // Vendor init + COLMOD + NORON + DISPON — no SWRESET
        _bus->batchOperation(xingzhi_vendor_init, sizeof(xingzhi_vendor_init));
        // invertDisplay() is called by the caller (main.cpp) after begin()
    }
};

static Arduino_DataBus *bus = new Arduino_ESP32SPI(
    LCD_DC, LCD_CS, LCD_SPI_CLK, LCD_SPI_MOSI);
// rotation=3 -> MADCTL MY+MV, which matches swap_xy + mirror_y.
// Arduino_TFT maps rotation=3 offsets as: xStart=ROW_OFFSET2, yStart=COL_OFFSET1.
// We need gap_x=36, gap_y=0 -> ROW_OFFSET2=36 and COL_OFFSET1=0.
static CalibratedST7789 *st7789_gfx = new CalibratedST7789(
    bus, LCD_RST,
    3,        // rotation: swap_xy + mirror_y -> landscape
    false,    // IPS = false
    240, 284, // native W×H (portrait); rotation=1 yields 284×240 logical
    0, 0,     // col_offset1, row_offset1
    0, 36);   // col_offset2, row_offset2
Arduino_GFX *gfx = st7789_gfx;

#else // Waveshare ESP32-S3-Touch-AMOLED-2.16
// Physical buttons (global, screen-independent):
//   BTN_BACK   (GPIO 0)  — left,  send Space (Claude Code voice mode push-to-talk)
//   BTN_FWD    (GPIO 18) — right, send Shift+Tab (Claude Code mode toggle)
//   AXP PWR    (PMU)     — middle, cycle screens; on splash, cycle animations
#define BTN_LEFT BTN_BACK
#define BTN_RIGHT BTN_FWD

static Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
static Arduino_CO5300 *co5300_gfx = new Arduino_CO5300(
    bus, LCD_RESET, 0 /* rotation */,
    LCD_WIDTH, LCD_HEIGHT, 0, 0, 0, 0);
Arduino_GFX *gfx = co5300_gfx;
TouchDrvCST92xx touch;
XPowersPMU pmu;
SensorQMI8658 imu;
#endif

static UsageData usage = {};

static const char *ble_state_name(ble_state_t s)
{
    switch (s)
    {
    case BLE_STATE_INIT:
        return "INIT";
    case BLE_STATE_ADVERTISING:
        return "ADVERTISING";
    case BLE_STATE_CONNECTED:
        return "CONNECTED";
    case BLE_STATE_DISCONNECTED:
        return "DISCONNECTED";
    default:
        return "UNKNOWN";
    }
}

#ifndef BOARD_XINGZHI_CUBE
// ---- Touch interrupt + shared state (Waveshare only) ----
static volatile bool touch_pressed = false;
static volatile uint16_t touch_x = 0;
static volatile uint16_t touch_y = 0;
static volatile bool touch_data_ready = false;

static void IRAM_ATTR touch_isr(void)
{
    touch_data_ready = true;
}

static void touch_read()
{
    if (!touch_data_ready)
        return;
    touch_data_ready = false;

    int16_t tx[5], ty[5];
    uint8_t n = touch.getPoint(tx, ty, touch.getSupportTouchPoint());
    if (n > 0)
    {
        touch_pressed = true;
        touch_x = (uint16_t)tx[0];
        touch_y = (uint16_t)ty[0];
    }
    else
    {
        touch_pressed = false;
    }
}
#endif // BOARD_XINGZHI_CUBE

// ---- LVGL draw buffers (PSRAM-backed, partial render) ----
#define BUF_LINES 40
static uint16_t *buf1 = nullptr;
static uint16_t *buf2 = nullptr;
#ifndef BOARD_XINGZHI_CUBE
// rot_buf for strip rotation — max size is 480×480 (full invalidation case)
static uint16_t *rot_buf = nullptr;
#endif
static bool lvgl_ready = false;

// LVGL tick callback
static uint32_t my_tick(void)
{
    return millis();
}

#ifndef BOARD_XINGZHI_CUBE
// Rotate a w×h strip and compute destination coordinates on the 480×480 display.
// src pixels are in row-major order for the rectangle (sx, sy, w, h).
// Output goes to rot_buf in row-major order for the destination rectangle.
static void rotate_strip(const uint16_t *src, int32_t w, int32_t h,
                         int32_t sx, int32_t sy, uint8_t r,
                         int32_t *dx, int32_t *dy, int32_t *dw, int32_t *dh)
{
    const int S = LCD_WIDTH; // 480

    switch (r)
    {
    case 1:
    { // 90° CW: (x,y) -> (S-1-y, x)
        *dw = h;
        *dh = w;
        *dx = S - sy - h;
        *dy = sx;
        for (int32_t y = 0; y < h; y++)
        {
            for (int32_t x = 0; x < w; x++)
            {
                // src(x,y) -> dst(h-1-y, x)
                rot_buf[x * h + (h - 1 - y)] = src[y * w + x];
            }
        }
        break;
    }
    case 2:
    { // 180°: (x,y) -> (S-1-x, S-1-y)
        *dw = w;
        *dh = h;
        *dx = S - sx - w;
        *dy = S - sy - h;
        for (int32_t y = 0; y < h; y++)
        {
            for (int32_t x = 0; x < w; x++)
            {
                rot_buf[(h - 1 - y) * w + (w - 1 - x)] = src[y * w + x];
            }
        }
        break;
    }
    case 3:
    { // 270° CW: (x,y) -> (y, S-1-x)
        *dw = h;
        *dh = w;
        *dx = sy;
        *dy = S - sx - w;
        for (int32_t y = 0; y < h; y++)
        {
            for (int32_t x = 0; x < w; x++)
            {
                // src(x,y) -> dst(y, w-1-x)
                rot_buf[(w - 1 - x) * h + y] = src[y * w + x];
            }
        }
        break;
    }
    default:
        *dx = sx;
        *dy = sy;
        *dw = w;
        *dh = h;
        break;
    }
}
#endif // BOARD_XINGZHI_CUBE

// ---- Backlight control (board-specific) ----
#ifdef BOARD_XINGZHI_CUBE
// LEDC PWM backlight on GPIO LCD_BL (0–255)
static void lcd_backlight_init(void)
{
    ledcAttach(LCD_BL, 5000, 8); // 5kHz, 8-bit resolution
    ledcWrite(LCD_BL, 0);
}
static void lcd_set_brightness(uint8_t val)
{
    ledcWrite(LCD_BL, val);
}
#else
// CO5300 has on-chip brightness register (0–255)
static void lcd_backlight_init(void) {} // nothing extra needed
static void lcd_set_brightness(uint8_t val)
{
    co5300_gfx->setBrightness(val);
}
#endif

// LVGL flush callback
static void my_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    int32_t w = area->x2 - area->x1 + 1;
    int32_t h = area->y2 - area->y1 + 1;
    uint16_t *src = (uint16_t *)px_map;
#ifndef BOARD_XINGZHI_CUBE
    // Waveshare: CPU-side strip rotation for auto-rotate via IMU
    uint8_t r = imu_get_rotation();
    if (r == 0)
    {
        gfx->draw16bitRGBBitmap(area->x1, area->y1, src, w, h);
    }
    else
    {
        int32_t dx, dy, dw, dh;
        rotate_strip(src, w, h, area->x1, area->y1, r, &dx, &dy, &dw, &dh);
        gfx->draw16bitRGBBitmap(dx, dy, rot_buf, dw, dh);
    }
#else
    // xingzhi-cube: fixed orientation, direct blit
    gfx->draw16bitRGBBitmap(area->x1, area->y1, src, w, h);
#endif
    lv_display_flush_ready(disp);
}

#ifndef BOARD_XINGZHI_CUBE
// CO5300 requires even-aligned flush regions
static void rounder_cb(lv_event_t *e)
{
    lv_area_t *area = (lv_area_t *)lv_event_get_param(e);
    area->x1 = area->x1 & ~1;
    area->y1 = area->y1 & ~1;
    area->x2 = area->x2 | 1;
    area->y2 = area->y2 | 1;
}

// LVGL touch callback (Waveshare only)
static void my_touch_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    if (touch_pressed)
    {
        data->point.x = touch_x;
        data->point.y = touch_y;
        data->state = LV_INDEV_STATE_PRESSED;
    }
    else
    {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}
#endif // BOARD_XINGZHI_CUBE

// Parse a JSON line into UsageData
static bool parse_json(const char *json, UsageData *out)
{
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err)
    {
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

// Serial command buffer
#define CMD_BUF_SIZE 64
static char cmd_buf[CMD_BUF_SIZE];
static int cmd_pos = 0;

static void send_screenshot()
{
    const uint32_t w = LCD_WIDTH, h = LCD_HEIGHT;
    const uint32_t row_bytes = w * 2;
    const uint32_t buf_size = row_bytes * h;
    uint8_t *sbuf = (uint8_t *)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!sbuf)
    {
        Serial.println("SCREENSHOT_ERR");
        return;
    }

    lv_draw_buf_t draw_buf;
    lv_draw_buf_init(&draw_buf, w, h, LV_COLOR_FORMAT_RGB565, row_bytes, sbuf, buf_size);

    lv_result_t res = lv_snapshot_take_to_draw_buf(lv_screen_active(), LV_COLOR_FORMAT_RGB565, &draw_buf);
    if (res != LV_RESULT_OK)
    {
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

static void check_serial_cmd()
{
    while (Serial.available())
    {
        char c = Serial.read();
        if (c == '\n' || c == '\r')
        {
            cmd_buf[cmd_pos] = '\0';
            if (strcmp(cmd_buf, "screenshot") == 0)
            {
                send_screenshot();
            }
            cmd_pos = 0;
        }
        else if (cmd_pos < CMD_BUF_SIZE - 1)
        {
            cmd_buf[cmd_pos++] = c;
        }
    }
}

void setup()
{
    Serial.begin(115200);
    delay(300);
    Serial.println("{\"ready\":true}");

#ifndef BOARD_XINGZHI_CUBE
    // Init I2C (shared by touch + PMU + IMU on Waveshare)
    Wire.begin(IIC_SDA, IIC_SCL);
#endif

    // Init display
    lcd_backlight_init();
    gfx->begin();
#ifdef BOARD_XINGZHI_CUBE
    // This panel's known-good setup uses inverted colors.
    gfx->invertDisplay(true);
#endif
    gfx->fillScreen(0x0000);
    lcd_set_brightness(200);

    // Init PMU (stubbed on xingzhi)
    power_init();

    // Init IMU for auto-rotation (stubbed on xingzhi — always returns 0)
    imu_init();

#ifndef BOARD_XINGZHI_CUBE
    // Init touch (Waveshare only)
    touch.setPins(TP_RST, TP_INT);
    if (!touch.begin(Wire, CST9220_ADDR, IIC_SDA, IIC_SCL))
    {
        Serial.println("Touch init failed");
    }
    else
    {
        touch.setMaxCoordinates(LCD_WIDTH, LCD_HEIGHT);
        touch.setSwapXY(true);
        touch.setMirrorXY(true, false);
        attachInterrupt(TP_INT, touch_isr, FALLING);
        Serial.println("Touch init OK");
    }
#endif

    // Init BLE data channel
    ble_init();
    Serial.printf("BLEDBG: mac=%s state=%s name=%s\n",
                  ble_get_mac_address(),
                  ble_state_name(ble_get_state()),
                  ble_get_device_name());

    // Init LVGL
    lv_init();
    lv_tick_set_cb(my_tick);

    // Allocate PSRAM-backed partial render buffers
    buf1 = (uint16_t *)heap_caps_malloc(LCD_WIDTH * BUF_LINES * 2, MALLOC_CAP_SPIRAM);
    buf2 = (uint16_t *)heap_caps_malloc(LCD_WIDTH * BUF_LINES * 2, MALLOC_CAP_SPIRAM);
#ifndef BOARD_XINGZHI_CUBE
    // rot_buf for IMU-driven strip rotation (Waveshare only)
    rot_buf = (uint16_t *)heap_caps_malloc(LCD_WIDTH * BUF_LINES * 2, MALLOC_CAP_SPIRAM);
#endif

    bool lvgl_buffers_ok = (buf1 && buf2);
#ifndef BOARD_XINGZHI_CUBE
    lvgl_buffers_ok = lvgl_buffers_ok && (rot_buf != nullptr);
#endif
    if (!lvgl_buffers_ok)
    {
        Serial.println("LVGL buffer alloc failed; entering BLE-only safe mode");
        gfx->fillScreen(0x0000);
        gfx->setTextColor(0x07E0);
        gfx->setTextSize(2);
        gfx->setCursor(8, 24);
        gfx->print("BLE safe mode");
        lvgl_ready = false;
    }
    else
    {
        lv_display_t *disp = lv_display_create(LCD_WIDTH, LCD_HEIGHT);
        lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
        lv_display_set_flush_cb(disp, my_flush_cb);
        lv_display_set_buffers(disp, buf1, buf2, LCD_WIDTH * BUF_LINES * 2,
                               LV_DISPLAY_RENDER_MODE_PARTIAL);

#ifndef BOARD_XINGZHI_CUBE
        // CO5300 requires even-aligned flush regions
        lv_display_add_event_cb(disp, rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);

        lv_indev_t *indev = lv_indev_create();
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(indev, my_touch_cb);
#endif
        lvgl_ready = true;
    }

    // Physical buttons
#ifdef BOARD_XINGZHI_CUBE
    pinMode(BTN_WAKE, INPUT_PULLUP);
    pinMode(BTN_VOL, INPUT_PULLUP);
    pinMode(BTN_MID, INPUT_PULLUP);
#else
    pinMode(BTN_BACK, INPUT_PULLUP);
    pinMode(BTN_FWD, INPUT_PULLUP);
#endif

    // Build dashboard
    if (lvgl_ready)
    {
        ui_init();

        // Show initial BLE status on Bluetooth screen
        ui_update_ble_status(ble_get_state(), ble_get_device_name(), ble_get_mac_address());

        // Show initial battery status
        ui_update_battery(power_battery_pct(), power_is_charging());
    }

#ifdef BOARD_XINGZHI_CUBE
    // Start on Usage for xingzhi so a dark splash palette can't look like a blank panel.
    if (lvgl_ready)
        ui_show_screen(SCREEN_USAGE);
#else
    if (lvgl_ready)
        ui_show_screen(SCREEN_SPLASH);
#endif

    Serial.println("Dashboard ready, waiting for data on BLE...");
}

static ble_state_t last_ble_state = BLE_STATE_INIT;
static uint32_t last_ble_dbg_ms = 0;

// Brightness ramp for rotation transition (Waveshare/CO5300 only)
// On rotation change: blank panel, force full LVGL redraw, ramp brightness back.
#ifndef BOARD_XINGZHI_CUBE
static void handle_rotation_change(void)
{
    static uint8_t last_rotation = 0;
    static uint8_t ramp_step = 0; // 0=idle, 1-4=ramping
    static uint32_t ramp_last = 0;

    uint8_t rot = imu_get_rotation();
    if (rot != last_rotation)
    {
        lcd_set_brightness(0);
        last_rotation = rot;
        lv_obj_invalidate(lv_screen_active());
        ramp_step = 1;
        return;
    }

    if (ramp_step == 0)
        return;
    uint32_t now = millis();
    if (now - ramp_last < 25)
        return;
    ramp_last = now;

    static const uint8_t levels[] = {60, 120, 170, 200};
    lcd_set_brightness(levels[ramp_step - 1]);
    if (ramp_step >= 4)
        ramp_step = 0;
    else
        ramp_step++;
}
#endif // BOARD_XINGZHI_CUBE

void loop()
{
#ifndef BOARD_XINGZHI_CUBE
    touch_read();
#endif
    if (lvgl_ready)
        lv_timer_handler();
    if (lvgl_ready)
        ui_tick_anim();
    ble_tick();
    power_tick();
    imu_tick();
    if (lvgl_ready)
        splash_tick();

    // Three-button input (global, screen-independent):
    //   LEFT   — Space (voice-mode push-to-talk; press & release tracked)
    //   RIGHT  — Shift+Tab (Claude Code mode toggle)
    //   MIDDLE — cycle screens; on splash, cycle animations
    //           (xingzhi: GPIO39 / Waveshare: AXP PMU short-press)
    {
        static bool back_was = false, fwd_was = false;
        bool back_now = (digitalRead(BTN_LEFT) == LOW);
        bool fwd_now = (digitalRead(BTN_RIGHT) == LOW);

        if (back_now != back_was)
        {
            if (back_now)
                ble_keyboard_press(0x2C, 0); // HID Space, no mods
            else
                ble_keyboard_release();
            back_was = back_now;
        }
        if (fwd_now != fwd_was)
        {
            if (fwd_now)
                ble_keyboard_press(0x2B, 0x02); // HID Tab + LEFT_SHIFT
            else
                ble_keyboard_release();
            fwd_was = fwd_now;
        }

#ifdef BOARD_XINGZHI_CUBE
        // xingzhi: GPIO39 (BTN_MID) always cycles screens so we can
        // recover even if splash content appears too dark.
        static bool mid_was = false;
        bool mid_now = (digitalRead(BTN_MID) == LOW);
        if (lvgl_ready && !mid_now && mid_was)
        { // trigger on release
            screen_t s = ui_get_current_screen();
            if (s == SCREEN_USAGE)
                ui_show_screen(SCREEN_BLUETOOTH);
            else if (s == SCREEN_BLUETOOTH)
                ui_show_screen(SCREEN_SPLASH);
            else
                ui_show_screen(SCREEN_USAGE);
        }
        mid_was = mid_now;
#else
        // Waveshare: AXP PWR short-press is the cycle button
        if (lvgl_ready && power_pwr_pressed())
        {
            if (ui_get_current_screen() == SCREEN_SPLASH)
                splash_next();
            else
                ui_cycle_screen();
        }
#endif // BOARD_XINGZHI_CUBE
    }

#ifndef BOARD_XINGZHI_CUBE
    handle_rotation_change();
#endif

    // Update BLE status on screen when state changes
    ble_state_t bs = ble_get_state();
    if (bs != last_ble_state)
    {
        Serial.printf("BLEDBG: state %s -> %s\n",
                      ble_state_name(last_ble_state),
                      ble_state_name(bs));
        last_ble_state = bs;
        if (lvgl_ready)
            ui_update_ble_status(bs, ble_get_device_name(), ble_get_mac_address());
    }

    uint32_t now_ms = millis();
    if (now_ms - last_ble_dbg_ms >= 10000)
    {
        last_ble_dbg_ms = now_ms;
        Serial.printf("BLEDBG: heartbeat state=%s mac=%s hasData=%d\n",
                      ble_state_name(bs),
                      ble_get_mac_address(),
                      ble_has_data() ? 1 : 0);
    }

    // Update battery indicator
    static int last_pct = -2;
    static bool last_charging = false;
    int pct = power_battery_pct();
    bool charging = power_is_charging();
    if (pct != last_pct || charging != last_charging)
    {
        last_pct = pct;
        last_charging = charging;
        if (lvgl_ready)
            ui_update_battery(pct, charging);
    }

    // Check for serial commands (screenshot, etc.)
    check_serial_cmd();

    // Process incoming BLE data
    if (ble_has_data())
    {
        if (parse_json(ble_get_data(), &usage))
        {
            int g_before = usage_rate_group();
            usage_rate_sample(usage.session_pct);
            int g_after = usage_rate_group();
            if (lvgl_ready && g_after != g_before)
            {
                Serial.printf("usage rate: group %d -> %d (s=%.2f%%)\n",
                              g_before, g_after, usage.session_pct);
                if (splash_is_active())
                    splash_pick_for_current_rate();
            }
            if (lvgl_ready)
                ui_update(&usage);
            ble_send_ack();
        }
        else
        {
            ble_send_nack();
        }
    }

    delay(5);
}
