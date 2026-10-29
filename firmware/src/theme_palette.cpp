#include "theme_palette.h"
#include <string.h>

// Anthropic-inspired dark palette, AMOLED-friendly (true black bg).
#define DEFAULTS                                                             \
    0x000000,   /* bg */                                                     \
    0x1f1f1e,   /* panel */                                                  \
    0xfaf9f5,   /* text */                                                   \
    0xb0aea5,   /* dim */                                                    \
    0xd97757,   /* accent: brand terra-cotta */                              \
    0x788c5d,   /* green */                                                  \
    0xd97757,   /* amber */                                                  \
    0xc0392b,   /* red */                                                    \
    0x2a2a28,   /* bar bg */                                                 \
    0xd97757,   /* face: Clawd orange */                                     \
    0x141413    /* eye */

const uint32_t THEME_DEFAULTS[TH_COUNT] = { DEFAULTS };
static uint32_t palette[TH_COUNT] = { DEFAULTS };

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool theme_parse_hex(const char* s, uint32_t out[TH_COUNT]) {
    if (!s || strlen(s) != THEME_HEX_LEN) return false;
    uint32_t p[TH_COUNT];
    for (int i = 0; i < TH_COUNT; i++) {
        uint32_t v = 0;
        for (int j = 0; j < 6; j++) {
            const int d = hex_digit(s[i * 6 + j]);
            if (d < 0) return false;
            v = (v << 4) | (uint32_t)d;
        }
        p[i] = v;
    }
    memcpy(out, p, sizeof(p));
    return true;
}

void theme_format_hex(const uint32_t in[TH_COUNT], char out[THEME_HEX_LEN + 1]) {
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < TH_COUNT; i++)
        for (int j = 0; j < 6; j++)
            out[i * 6 + j] = digits[(in[i] >> (20 - 4 * j)) & 0xf];
    out[THEME_HEX_LEN] = '\0';
}

uint32_t theme_hex(ThemeColor c) { return c < TH_COUNT ? palette[c] : 0; }

bool theme_palette_set(const uint32_t p[TH_COUNT]) {
    if (memcmp(palette, p, sizeof(palette)) == 0) return false;
    memcpy(palette, p, sizeof(palette));
    return true;
}
