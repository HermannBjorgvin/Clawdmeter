#include "../../hal/touch_hal.h"
#include "ft6336.h"

// The panel part of the touch screen. Touches that start in the button strip
// below the panel go to input.cpp / power.cpp instead (ft6336.cpp).

void touch_hal_init(void) { ft6336_init(); }

void touch_hal_read(uint16_t* x, uint16_t* y, bool* pressed) {
    ft6336_poll();
    *pressed = ft6336_zone() == TZ_SCREEN;
    *x = ft6336_x();
    *y = ft6336_y();
}
