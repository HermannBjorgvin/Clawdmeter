// USB CDC serial link to the host daemon.
// Replaces the old BLE GATT transport. Owns all Serial I/O including the
// existing `screenshot` debug command.
//
// Wire protocol (line-delimited, '\n' terminator, host ↔ device):
//   host → device : {"s":..,"sr":..,"w":..,"wr":..,"st":"..","ok":..}  usage payload
//   host → device : screenshot                                          dump framebuffer
//   device → host : ACK                                                  payload accepted
//   device → host : NACK                                                 payload parse failed
//   device → host : REQ                                                  please send fresh data
//   device → host : SCREENSHOT_{START,END,ERR}                           framebuffer dump frames

#include "serial_link.h"
#include <Arduino.h>
#include <string.h>
#include <lvgl.h>
#include "display_cfg.h"

// Sized for PR #22 Activity payload (~500-700 bytes worst case with 3
// sessions x multiple todos). PR #26 default of 192 was tuned for Usage-only
// payloads (~120 bytes); anything larger silently dropped.
#define LINE_BUF_SIZE   4096
#define DATA_BUF_SIZE   4096
#define LINK_STALE_MS   5000   // mark link stale if no data for this long

static char     line_buf[LINE_BUF_SIZE];
static int      line_pos = 0;

static char     data_buf[DATA_BUF_SIZE];
static bool     has_data       = false;
static uint32_t last_data_ms   = 0;
static bool     ever_received  = false;

// Framebuffer screenshot — moved here from main.cpp so all Serial I/O lives
// in one place. Unchanged otherwise.
static void send_screenshot() {
    const uint32_t w = LCD_WIDTH, h = LCD_HEIGHT;
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

static void handle_line(const char* line) {
    if (line[0] == '{') {
        // JSON usage payload — copy into data_buf and flag for main loop.
        strncpy(data_buf, line, DATA_BUF_SIZE - 1);
        data_buf[DATA_BUF_SIZE - 1] = '\0';
        has_data      = true;
        last_data_ms  = millis();
        ever_received = true;
    } else if (strcmp(line, "screenshot") == 0) {
        send_screenshot();
    }
    // Unknown lines silently ignored.
}

void serial_link_init(void) {
    line_pos = 0;
    has_data = false;
    // Announce readiness so the daemon knows the device is alive after
    // boot (or after a USB CDC re-enumeration).
    Serial.println("READY");
}

void serial_link_tick(void) {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            if (line_pos > 0) {
                line_buf[line_pos] = '\0';
                handle_line(line_buf);
            }
            line_pos = 0;
        } else if (line_pos < LINE_BUF_SIZE - 1) {
            line_buf[line_pos++] = c;
        } else {
            // Overflow — discard line and resync at next newline.
            line_pos = 0;
        }
    }
}

link_state_t serial_link_get_state(void) {
    if (!ever_received) return LINK_STATE_WAITING;
    if ((millis() - last_data_ms) > LINK_STALE_MS) return LINK_STATE_STALE;
    return LINK_STATE_CONNECTED;
}

const char* serial_link_get_port_name(void) {
    return "USB CDC";  // /dev/ttyACM0 on the host
}

bool serial_link_has_data(void) {
    return has_data;
}

const char* serial_link_get_data(void) {
    has_data = false;
    return data_buf;
}

void serial_link_send_ack(void)             { Serial.println("ACK"); }
void serial_link_send_nack(void)            { Serial.println("NACK"); }
void serial_link_request_refresh(void)      { Serial.println("REQ"); }
