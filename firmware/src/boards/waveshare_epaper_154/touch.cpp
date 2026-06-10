#include "../../hal/touch_hal.h"

// No touch panel on this SKU.
void touch_hal_init(void) {}

void touch_hal_read(uint16_t* x, uint16_t* y, bool* pressed) {
    if (x)       *x = 0;
    if (y)       *y = 0;
    if (pressed) *pressed = false;
}
