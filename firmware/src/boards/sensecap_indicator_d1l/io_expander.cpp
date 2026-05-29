#include "io_expander.h"
#include "board.h"
#include <Arduino.h>
#include <Wire.h>

bool io_expander_init(void) { return true; }
void io_expander_set(uint8_t pin, bool high) { (void)pin; (void)high; }
bool io_expander_get(uint8_t pin) { (void)pin; return false; }
