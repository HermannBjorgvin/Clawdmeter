#pragma once

// Auto-sleep / idle screen-off configuration.
// All tunables live here so nothing is hard-coded in main.cpp / idle.cpp.

#define IDLE_TIMEOUT_MS             (30UL * 60UL * 1000UL)  // 30 min
#define IDLE_FADE_OUT_MS            400      // fade-to-black duration
#define IDLE_FADE_IN_MS             180      // wake fade-in (snappier)
#define IDLE_FADE_STEP_MS           20       // tick interval per fade step

#define DISPLAY_DEFAULT_BRIGHTNESS  200      // active-screen brightness

// When false, the device never enters sleep while USB power is present (also
// wakes from sleep when USB is plugged back in). Useful when sitting on a
// desk plugged in — also covers battery-less hardware that's always on USB.
// Set true to sleep regardless of power source.
#define IDLE_SLEEP_WHEN_CHARGING    false

// When true, a touch on the dark panel wakes the device (first touch is
// consumed for wake only, second touch acts normally). When false, touch is
// fully ignored during sleep — useful if cats/sleeves brushing the panel
// overnight would be a problem.
#define IDLE_WAKE_ON_TOUCH          true
