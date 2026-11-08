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
bool sim_vbus_in(void);     // USB power present (charging, or no battery fitted)

// Scenario playback controls, implemented in ble_sim.cpp.
void sim_playback_toggle(void);
void sim_playback_step(int dir);
void sim_playback_jump(int idx);      // 0-based
void sim_playback_toggle_link(void);  // BLE connected <-> disconnected

// Fast-clock injection (keys e / t / a / m), implemented in ble_sim.cpp.
void sim_inject_event(char ev);         // 'p' prompt, 's' stop
void sim_inject_activity_cycle(void);   // a = 0 → 1 → 2 → 3 → 0
void sim_inject_alert_toggle(void);     // attention alert / clear

// Scripted touch for headless runs (SIM_TAPS), read by touch.cpp.
bool sim_scripted_touch(uint16_t* x, uint16_t* y);

// PWR button and battery, set by the keyboard or a controller.
void sim_set_pwr(bool down);
void sim_set_battery(int pct, bool charging, bool usb_only);

// Controller mode (sim_ctrl.cpp): with SIM_CTRL_FD set, a host (tools/simgrid)
// drives the sim over that socket. Time is virtual and moves only when the
// host says so, and touch, buttons, the BLE link and payloads come from it.
bool sim_ctrl_active(void);
void sim_ctrl_run(void);                        // replaces the free-running loop
bool sim_ctrl_touch(uint16_t* x, uint16_t* y);  // finger down, and where
bool sim_ctrl_button(int btn);                  // InputButton held

// BLE stand-in hooks for the controller (ble_sim.cpp).
void sim_ble_push(const char* json);   // deliver a payload, as the app's write
void sim_ble_set_link(bool connected);

// The panel as last flushed (display.cpp): RGB565, brightness 0..255, and a
// counter that moves whenever pixels or brightness change.
const uint16_t* sim_display_pixels(uint8_t* brightness, uint32_t* version);
