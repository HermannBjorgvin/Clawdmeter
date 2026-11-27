#include "../../hal/touch_hal.h"

// The non-touch Waveshare board has no touch controller. Report "never
// pressed" so the shared input path stays inert.

void touch_hal_init(void) {}

void touch_hal_read(uint16_t* x, uint16_t* y, bool* pressed) {
    *x = 0;
    *y = 0;
    *pressed = false;
}
