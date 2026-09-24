#include "board.h"
#include "../../hal/touch_hal.h"
#include <SDL.h>

void touch_hal_init(void) {}

// Mouse position + left button = one finger. Events are pumped every loop
// by sim_main.cpp, so SDL_GetMouseState is always fresh here.
void touch_hal_read(uint16_t* x, uint16_t* y, bool* pressed) {
    int mx, my;
    uint32_t b = SDL_GetMouseState(&mx, &my);
    if (mx < 0) mx = 0;
    if (mx >= LCD_WIDTH) mx = LCD_WIDTH - 1;
    if (my < 0) my = 0;
    if (my >= LCD_HEIGHT) my = LCD_HEIGHT - 1;
    *x = (uint16_t)mx;
    *y = (uint16_t)my;
    *pressed = (b & SDL_BUTTON(SDL_BUTTON_LEFT)) != 0;
}
