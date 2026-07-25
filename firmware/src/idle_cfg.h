#pragma once

// Auto-sleep / idle screen-off configuration.
// All tunables live here so nothing is hard-coded in main.cpp / idle.cpp.

// Sleep is driven by TOKEN activity, not input activity: every payload whose
// usage/context numbers changed calls idle_note_activity() (see main.cpp), so
// the panel stays lit while Claude is actually burning tokens and goes dark
// this long after the last movement. User input still counts as activity.
#define IDLE_TIMEOUT_MS             (10UL * 60UL * 1000UL)  // 10 min
#define IDLE_FADE_OUT_MS            400      // fade-to-black duration
#define IDLE_FADE_IN_MS             180      // wake fade-in (snappier)
#define IDLE_FADE_STEP_MS           20       // tick interval per fade step

#define DISPLAY_DEFAULT_BRIGHTNESS  200      // active-screen brightness

// True: sleep regardless of power source. This is the desk-monitor behaviour
// we want — the device is normally on USB, and "stays lit all night because
// it's plugged in" defeats the token-activity screensaver above. Token
// movement (or any button/touch) lights it straight back up.
#define IDLE_SLEEP_WHEN_CHARGING    true

// When true, a touch on the dark panel wakes the device (first touch is
// consumed for wake only, second touch acts normally). When false, touch is
// fully ignored during sleep — useful if cats/sleeves brushing the panel
// overnight would be a problem.
#define IDLE_WAKE_ON_TOUCH          true
