#include "board.h"
#include "../../hal/display_hal.h"
#include "sim_platform.h"
#include <SDL.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static SDL_Window*   win = NULL;
static SDL_Renderer* ren = NULL;
static SDL_Texture*  tex = NULL;
static uint16_t*     shadow = NULL;   // full-frame RGB565 — what the panel "shows"
static uint8_t       brightness = 200;
static bool          dirty = false;

static void present(void) {
    SDL_UpdateTexture(tex, NULL, shadow, LCD_WIDTH * 2);
    // AMOLED brightness → linear dim of the whole frame.
    SDL_SetTextureColorMod(tex, brightness, brightness, brightness);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, tex, NULL, NULL);
    SDL_RenderPresent(ren);
    dirty = false;
}

void display_hal_init(void) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        exit(1);
    }
    win = SDL_CreateWindow("Clawdmeter sim",
                           SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           LCD_WIDTH, LCD_HEIGHT, 0);
    ren = SDL_CreateRenderer(win, -1, 0);
    if (!win || !ren) {
        fprintf(stderr, "SDL window/renderer failed: %s\n", SDL_GetError());
        exit(1);
    }
    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB565,
                            SDL_TEXTUREACCESS_STREAMING, LCD_WIDTH, LCD_HEIGHT);
    shadow = (uint16_t*)calloc((size_t)LCD_WIDTH * LCD_HEIGHT, 2);
}

void display_hal_begin(void) { present(); }

void display_hal_set_brightness(uint8_t level) {
    if (level == brightness) return;
    brightness = level;
    // Present immediately so idle fades stay visible even while main.cpp
    // skips display_hal_tick() (asleep).
    present();
}

void display_hal_fill_screen(uint16_t color565) {
    for (int i = 0; i < LCD_WIDTH * LCD_HEIGHT; i++) shadow[i] = color565;
    dirty = true;
}

void display_hal_draw_bitmap(int32_t x, int32_t y, int32_t w, int32_t h,
                             const uint16_t* pixels) {
    for (int32_t row = 0; row < h; row++) {
        int32_t dy = y + row;
        if (dy < 0 || dy >= LCD_HEIGHT) continue;
        int32_t sx = x < 0 ? -x : 0;
        int32_t cw = w - sx;
        if (x + sx + cw > LCD_WIDTH) cw = LCD_WIDTH - (x + sx);
        if (cw <= 0) continue;
        memcpy(&shadow[dy * LCD_WIDTH + x + sx], &pixels[(size_t)row * w + sx],
               (size_t)cw * 2);
    }
    dirty = true;
}

void display_hal_tick(void) {
    if (dirty) present();
}

void display_hal_round_area(int32_t* x1, int32_t* y1, int32_t* x2, int32_t* y2) {
    (void)x1; (void)y1; (void)x2; (void)y2;  // no controller alignment quirks
}

void sim_display_set_title(const char* title) {
    if (win) SDL_SetWindowTitle(win, title);
}

void sim_display_screenshot(const char* path) {
    char buf[64];
    if (!path) {
        static int n = 0;
        snprintf(buf, sizeof(buf), "sim-shot-%d.bmp", n++);
        path = buf;
    }
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormatFrom(
        shadow, LCD_WIDTH, LCD_HEIGHT, 16, LCD_WIDTH * 2, SDL_PIXELFORMAT_RGB565);
    if (s && SDL_SaveBMP(s, path) == 0) printf("screenshot: %s\n", path);
    else printf("screenshot failed: %s\n", SDL_GetError());
    if (s) SDL_FreeSurface(s);
}
