#pragma once

// Native desktop simulator "board" — an SDL2 window standing in for the
// 480×480 AMOLED so shared code (main.cpp, ui.cpp, splash.cpp) can be
// developed without hardware. No pins here, just geometry and the key map.
//
//   mouse / left-drag    touch (on the meters he reacts; elsewhere "show me the meters")
//   space                play/pause scenario playback
//   left / right         step one scenario state (pauses playback)
//   1..9                 jump to scenario state N (pauses playback)
//   d                    toggle BLE connected/disconnected
//   b (hold)             PRIMARY button  (BOOT — HID Space PTT on hardware)
//   n (hold)             SECONDARY button (HID Shift+Tab on hardware)
//   p                    PWR button (short press; hold ~3s + release = pair)
//   c                    toggle charging       - / =   battery down / up 5%
//   u                    toggle no battery (USB power only → plug-zap icon)
//   e / t                inject a hook event: prompt / stop
//   a                    inject activity level, cycling 0 → 1 → 2 → 3
//   m                    inject an attention alert / clear it
//   s                    save screenshot BMP to the current directory
//   esc / window close   quit
//
// Scenario: sim/scenario.jsonl (relative to the firmware/ dir), overridable
// with SIM_SCENARIO=<path>. One JSON object per line — the device payload
// plus optional "name" and "hold_ms" (default 3000). Lines starting with #
// are comments. Missing file → a small built-in state list.
//
// Headless / CI: SDL_VIDEODRIVER=dummy SIM_AUTOSHOT_MS=<ms> saves a
// screenshot (SIM_AUTOSHOT_PATH, default sim-autoshot.bmp) after <ms> and
// exits. SIM_SHOTS="ms:path,ms:path" saves several and exits after the last;
// SIM_TAPS="ms@x,y;ms@x,y+hold" presses the panel at those times (120 ms, or
// `hold` ms); SIM_NO_PWR=1 plays a board without a PWR button; SIM_FAST=1
// compresses the director's minutes into seconds (sim/director.jsonl);
// SIM_MINIMAL=1 plays a minimal-mode board. SIM_CMDS="ms:cmd;ms:cmd" types
// serial commands at those times (`pwr`, `prompt`, `stop`, `alert` are the
// PWR button, the e / t hook events and the m alert toggle). SIM_TRACE=1 prints every director step.

#define BOARD_NAME  "Simulator 480x480"
// Panel size; override to preview other boards' geometry, e.g.
//   PLATFORMIO_BUILD_FLAGS="-DSIM_LCD_W=368 -DSIM_LCD_H=448" pio run -e sim
#ifdef SIM_LCD_W
#define LCD_WIDTH   SIM_LCD_W
#define LCD_HEIGHT  SIM_LCD_H
#else
#define LCD_WIDTH   480
#define LCD_HEIGHT  480
#endif
