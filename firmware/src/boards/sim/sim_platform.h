#pragma once
#include <stdint.h>

// Glue between the SDL event pump and the individual HAL stubs. Everything
// is single-threaded — sim_pump() runs once per loop() iteration, called
// from sim_main.cpp before loop().

void sim_pump(void);
bool sim_should_quit(void);

// Implemented in display.cpp (owns the SDL window).
void sim_display_set_title(const char* title);
void sim_display_screenshot(const char* path);  // NULL → auto-numbered sim-shot-N.bmp

// PWR button edges + fake battery, consumed by power.cpp.
bool sim_take_pwr_pressed(void);
bool sim_take_pwr_long(void);
bool sim_take_pwr_released(void);
int  sim_battery_pct(void);
bool sim_charging(void);

// Scenario playback controls, implemented in ble_sim.cpp.
void sim_playback_toggle(void);
void sim_playback_step(int dir);
void sim_playback_jump(int idx);      // 0-based
void sim_playback_toggle_link(void);  // BLE connected <-> disconnected
