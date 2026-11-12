#include "../../hal/touch_hal.h"

// The non-touch Freenove variant has no touch controller. Report "never
// pressed" so the shared input path stays inert. (The touch variant,
// FNK0104B, carries an FT6336 on I2C and would need its own board folder.)

void touch_hal_init(void) {}

void touch_hal_read(uint16_t* x, uint16_t* y, bool* pressed) {
    *x = 0;
    *y = 0;
    *pressed = false;
}
