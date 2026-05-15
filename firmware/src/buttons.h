#pragma once
//
// Physical button input layer.
//
// Each board owns its own mapping of buttons to actions; this header
// declares the small generic API that main.cpp calls. Implementations
// live in buttons.cpp behind BOARD_* guards.
//
// Action vocabulary (chosen so both boards converge on the same UI
// behavior):
//   - voice push-to-talk    — Space held while a button is held
//   - mode toggle           — Shift+Tab edge-triggered
//   - cycle screen          — Usage <-> Bluetooth (or, on splash, cycle
//                             through the pixel-art animations)
//   - back / splash toggle  — toggle to/from the splash screen
//

void buttons_init(void);
void buttons_tick(void);
