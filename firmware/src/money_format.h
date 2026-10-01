#pragma once
#include <stdio.h>
#include <string.h>

// Pure money formatting for the Enterprise Spending card. Free of LVGL/Arduino
// so it can be unit-tested on the host (see test/test_money_format/).

// Whole currency units with thousands separators: "$12", "$1,500", "EUR 1,250".
// USD gets "$"; other codes are prefixed as-is (the bitmap fonts are ASCII-only).
static inline void money_prefix(const char* currency, char* out, size_t n) {
    if (!currency || !*currency || strcmp(currency, "USD") == 0) snprintf(out, n, "$");
    else                                                         snprintf(out, n, "%s ", currency);
}

static inline void money_format_whole(long minor, int exponent, const char* currency,
                                      char* out, size_t n) {
    long whole = minor < 0 ? 0 : minor;
    for (int i = 0; i < exponent; i++) whole /= 10;

    char digits[24];
    snprintf(digits, sizeof(digits), "%ld", whole);
    char grouped[32];
    size_t len = strlen(digits), j = 0;
    for (size_t i = 0; i < len && j < sizeof(grouped) - 1; i++) {
        if (i > 0 && (len - i) % 3 == 0) grouped[j++] = ',';
        grouped[j++] = digits[i];
    }
    grouped[j] = '\0';

    char prefix[8];
    money_prefix(currency, prefix, sizeof(prefix));
    snprintf(out, n, "%s%s", prefix, grouped);
}

// Compact fallback when the whole amount won't fit: "$17.4k", "EUR 1.2M" (floored).
static inline void money_format_short(long minor, int exponent, const char* currency,
                                      char* out, size_t n) {
    long whole = minor < 0 ? 0 : minor;
    for (int i = 0; i < exponent; i++) whole /= 10;
    char prefix[8];
    money_prefix(currency, prefix, sizeof(prefix));
    if (whole >= 1000000)   snprintf(out, n, "%s%ld.%ldM", prefix, whole / 1000000, (whole / 100000) % 10);
    else if (whole >= 1000) snprintf(out, n, "%s%ld.%ldk", prefix, whole / 1000, (whole / 100) % 10);
    else                    snprintf(out, n, "%s%ld", prefix, whole);
}
