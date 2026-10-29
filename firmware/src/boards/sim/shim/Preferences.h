#pragma once
// In-memory stand-in for ESP32 NVS Preferences (used by brightness.cpp).
// State lasts for the process lifetime only — the sim boots with defaults.
#include <stdint.h>
#include <stddef.h>

class Preferences {
public:
    bool begin(const char* name, bool read_only = false) {
        (void)name; (void)read_only;
        return true;
    }
    void end(void) {}
    uint8_t getUChar(const char* key, uint8_t def = 0);
    size_t  putUChar(const char* key, uint8_t value);
    bool    isKey(const char* key);
    size_t  getBytes(const char* key, void* buf, size_t max_len);
    size_t  putBytes(const char* key, const void* value, size_t len);
};
