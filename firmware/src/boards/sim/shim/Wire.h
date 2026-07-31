#pragma once
// The sim has no I2C bus. main.cpp includes <Wire.h> but never touches it —
// all Wire usage lives in the hardware boards' board_init.cpp files.
