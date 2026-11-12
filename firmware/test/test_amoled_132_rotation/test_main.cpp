// Host unit test for the AMOLED-1.32 button rotation: touch must map panel
// coordinates back through exactly the rotation the display applied.
//
//   g++ -std=c++17 -I ../../src/boards/waveshare_amoled_132 test_main.cpp -o t && ./t

#include "rotation_map.h"
#include <cstdio>
#include <cstring>

static int failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);             \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

static const uint16_t S = 466;

static void should_round_trip_every_pixel_in_every_quadrant() {
    for (uint8_t q = 0; q < 4; q++) {
        bool ok = true;
        for (uint16_t y = 0; y < S && ok; y++)
            for (uint16_t x = 0; x < S && ok; x++) {
                uint16_t px, py, rx, ry;
                rot_forward(q, S, x, y, &px, &py);
                ok = px < S && py < S;
                rot_inverse(q, S, px, py, &rx, &ry);
                ok = ok && rx == x && ry == y;
            }
        CHECK(ok);
    }
}

static void should_turn_clockwise_one_quarter_per_step() {
    uint16_t px, py;
    rot_forward(1, S, 0, 0, &px, &py);          // top-left -> top-right
    CHECK(px == S - 1 && py == 0);
    rot_forward(2, S, 0, 0, &px, &py);          // top-left -> bottom-right
    CHECK(px == S - 1 && py == S - 1);
    rot_forward(3, S, 0, 0, &px, &py);          // top-left -> bottom-left
    CHECK(px == 0 && py == S - 1);
}

// Mirror of display.cpp rotate_strip()'s buffer indexing: the pixel written at
// (dx + col, dy + row) must be the one rot_forward says lands there.
static void should_match_display_strip_rotation() {
    const int32_t sx = 10, sy = 40, w = 6, h = 4;
    for (uint8_t r = 1; r < 4; r++) {
        uint32_t src[w * h], dst[w * h];
        for (int32_t i = 0; i < w * h; i++) src[i] = i;
        int32_t dx, dy, dw;
        switch (r) {
        case 1: dw = h; dx = S - sy - h; dy = sx;
            for (int32_t y = 0; y < h; y++) for (int32_t x = 0; x < w; x++)
                dst[x * h + (h - 1 - y)] = src[y * w + x];
            break;
        case 2: dw = w; dx = S - sx - w; dy = S - sy - h;
            for (int32_t y = 0; y < h; y++) for (int32_t x = 0; x < w; x++)
                dst[(h - 1 - y) * w + (w - 1 - x)] = src[y * w + x];
            break;
        default: dw = h; dx = sy; dy = S - sx - w;
            for (int32_t y = 0; y < h; y++) for (int32_t x = 0; x < w; x++)
                dst[(w - 1 - x) * h + y] = src[y * w + x];
            break;
        }
        bool ok = true;
        for (int32_t y = 0; y < h; y++)
            for (int32_t x = 0; x < w; x++) {
                uint16_t px, py;
                rot_forward(r, S, sx + x, sy + y, &px, &py);
                ok = ok && dst[(py - dy) * dw + (px - dx)] == (uint32_t)(y * w + x);
            }
        CHECK(ok);
    }
}

int main() {
    should_round_trip_every_pixel_in_every_quadrant();
    should_turn_clockwise_one_quarter_per_step();
    should_match_display_strip_rotation();
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("all rotation tests passed\n");
    return 0;
}
