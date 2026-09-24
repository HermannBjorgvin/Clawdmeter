#include <Arduino.h>
#include <Preferences.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

unsigned long millis(void) {
    static uint64_t t0 = now_ms();
    return (unsigned long)(now_ms() - t0);
}

void delay(unsigned long ms) { usleep(ms * 1000); }

size_t SimSerial::write(const uint8_t* buf, size_t len) {
    return fwrite(buf, 1, len, stdout);
}
void SimSerial::printf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}
SimSerial Serial;

// ---- Preferences: tiny in-memory key/value store ----
#define PREF_SLOTS 16
static struct { char key[16]; uint8_t val; bool used; } store[PREF_SLOTS];

uint8_t Preferences::getUChar(const char* key, uint8_t def) {
    for (auto& s : store)
        if (s.used && strcmp(s.key, key) == 0) return s.val;
    return def;
}
size_t Preferences::putUChar(const char* key, uint8_t value) {
    for (auto& s : store)
        if (s.used && strcmp(s.key, key) == 0) { s.val = value; return 1; }
    for (auto& s : store)
        if (!s.used) {
            s.used = true;
            strncpy(s.key, key, sizeof(s.key) - 1);
            s.val = value;
            return 1;
        }
    return 0;
}
