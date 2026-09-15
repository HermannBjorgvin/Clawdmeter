#pragma once
#include <ArduinoJson.h>
#include <stdint.h>

// Implemented in main.cpp — exposes device state to the HTTP layer
// (web_server.cpp) without moving the canonical data structs out of
// main.cpp, where they're already parsed from BLE/serial payloads.

// Serializes everything the device currently holds (usage, copilot,
// sysinfo, vscode, env/weather, aurora, ci, today, activity, ble/wifi
// status) into `doc` for the GET /api/state endpoint.
void build_state_json(JsonDocument& doc);

// Captures the current LVGL frame into `out`, which must point at
// LCD_WIDTH*LCD_HEIGHT uint16_t RGB565 pixels. Used by GET /api/screenshot.bmp.
bool capture_screenshot_rgb565(uint16_t* out);
