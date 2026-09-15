#include "web_server.h"
#include <WebServer.h>
#include <ArduinoJson.h>
#include "device_api.h"
#include "display_cfg.h"
#include "web_ui.h"
#include "wifi_net.h"

static WebServer server(80);

static void handle_root() {
    server.sendHeader("Content-Encoding", "gzip");
    server.send_P(200, "text/html", (PGM_P)INDEX_HTML_GZ, INDEX_HTML_GZ_LEN);
}

static void handle_state() {
    JsonDocument doc;
    build_state_json(doc);
    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
}

static void write_le16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void write_le32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

// Captures the current frame and streams it as an uncompressed 24-bpp BMP —
// any browser renders that with a plain <img>, no client-side decode needed.
// Converts RGB565 -> BGR888 one row at a time so we never need a second
// full-size buffer alongside the RGB565 capture (peak cost stays at one
// LCD_WIDTH*LCD_HEIGHT*2 transient allocation, freed before this returns).
static void handle_screenshot() {
    uint16_t* fb = (uint16_t*)malloc((size_t)LCD_WIDTH * LCD_HEIGHT * 2);
    if (!fb) {
        server.send(500, "text/plain", "out of memory");
        return;
    }
    capture_screenshot_rgb565(fb);

    const int w = LCD_WIDTH, h = LCD_HEIGHT;
    const int row_bytes = ((w * 3 + 3) / 4) * 4;  // BMP rows padded to a 4-byte boundary
    const uint32_t pixel_data_size = (uint32_t)row_bytes * (uint32_t)h;
    const uint32_t file_size = 54 + pixel_data_size;

    uint8_t hdr[54] = {0};
    hdr[0] = 'B'; hdr[1] = 'M';
    write_le32(&hdr[2],  file_size);
    write_le32(&hdr[10], 54);           // pixel data offset
    write_le32(&hdr[14], 40);           // DIB header size (BITMAPINFOHEADER)
    write_le32(&hdr[18], (uint32_t)w);
    write_le32(&hdr[22], (uint32_t)h);  // positive height = bottom-up rows
    write_le16(&hdr[26], 1);            // planes
    write_le16(&hdr[28], 24);           // bpp
    write_le32(&hdr[34], pixel_data_size);

    server.setContentLength(file_size);
    server.send(200, "image/bmp", "");
    server.sendContent((const char*)hdr, sizeof(hdr));

    uint8_t row[LCD_WIDTH * 3 + 3] = {0};
    for (int y = h - 1; y >= 0; y--) {   // BMP stores rows bottom-up
        for (int x = 0; x < w; x++) {
            uint16_t px = fb[y * w + x];
            uint8_t r = (px >> 11) & 0x1F, g = (px >> 5) & 0x3F, b = px & 0x1F;
            row[x * 3 + 0] = (uint8_t)((b * 255) / 31);  // BMP pixel order is BGR
            row[x * 3 + 1] = (uint8_t)((g * 255) / 63);
            row[x * 3 + 2] = (uint8_t)((r * 255) / 31);
        }
        server.sendContent((const char*)row, row_bytes);
    }
    free(fb);
}

static bool started = false;

void web_server_init(void) {
    // Routes only -- server.begin() is deferred to web_server_tick() below,
    // once WiFi actually has a real IP. Calling WebServer::begin() (which
    // creates+binds+listens a socket) before that crashes on this
    // esp32-arduino version (lwIP's tcpip thread isn't fully up) or hangs
    // handleClient() indefinitely (interface exists but has no IP yet) --
    // this is also just the standard, widely-used ESP32 Arduino pattern.
    server.on("/", HTTP_GET, handle_root);
    server.on("/api/state", HTTP_GET, handle_state);
    server.on("/api/screenshot.bmp", HTTP_GET, handle_screenshot);
}

void web_server_tick(void) {
    if (!started) {
        if (wifi_get_state() != WIFI_STATE_CONNECTED) return;
        server.begin();
        started = true;
    }
    server.handleClient();
}
