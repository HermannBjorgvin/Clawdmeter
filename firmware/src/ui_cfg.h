#pragma once

// Usage-screen styling tunables. Kept here so nothing cosmetic is hard-coded
// in ui.cpp — mirrors the idle_cfg.h / theme.h "single source of truth" pattern.

// ---- "Extra Usage" (overage) title treatment ----
// When 1, the overage screen draws the title inside a filled banner (the owner's
// preferred look). When 0, the title is plain text on the AMOLED-black
// background, consistent with the normal "Usage" screen and the brand palette.
#define UI_EXTRA_USAGE_BANNER        1

// Banner fill + text colors (only used when UI_EXTRA_USAGE_BANNER is 1).
// Dark text on the yellow fill keeps the title legible; pure-red text on yellow
// reads poorly, so the banner overrides the normal red title color.
#define UI_EXTRA_USAGE_BANNER_COLOR  0xe8b339   // amber-yellow banner fill
#define UI_EXTRA_USAGE_BANNER_TEXT   0x1a1a18   // near-black title text on banner
