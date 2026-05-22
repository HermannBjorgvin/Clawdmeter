#pragma once
#include <stdint.h>

// Touch abstraction. The board owns the touch controller driver and the
// TP_INT pin wiring. The HAL implementation is responsible for keeping its
// own internal "latest sample" state — shared code calls touch_hal_read()
// once per loop and feeds it into LVGL.
//
// Implementations should complete touch_hal_read() in well under 5 ms (a
// single I2C burst). LVGL polls this at the screen refresh rate.

void touch_hal_init(void);

// Pump the controller and return the latest sample. *pressed reflects
// whether any finger is currently down; coordinates are valid only when
// pressed is true and are in display (post-orientation) coordinates.
void touch_hal_read(uint16_t* x, uint16_t* y, bool* pressed);
