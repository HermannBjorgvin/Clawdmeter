#pragma once
// Host stand-in for the Arduino core, for the director test
// (test/test_director). The test owns the clock.
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

extern uint32_t fake_now_ms;
extern bool     fake_serial_echo;
static inline unsigned long millis(void) { return fake_now_ms; }

struct FakeSerial {
    void printf(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        if (!fake_serial_echo) return;
        va_list ap;
        va_start(ap, fmt);
        vprintf(fmt, ap);
        va_end(ap);
    }
    void println(const char* s) { if (fake_serial_echo) puts(s); }
};
extern FakeSerial Serial;
