#pragma once
#include <stdint.h>

bool io_expander_init(void);
void io_expander_set(uint8_t pin, bool high);
bool io_expander_get(uint8_t pin);
