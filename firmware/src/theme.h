#pragma once
#include <lvgl.h>
#include <stdint.h>

// Design tokens — single source of truth for UI colors.
//
// Petmeter shows more than one provider's usage, and each gets its own
// palette so a glance at the screen tells you which plan you are looking at.
// Colors are stored as plain hex rather than lv_color_t so the tables stay
// constant-initialized: lv_color_hex() is a function, and a table of calls
// would run at static-init time, before LVGL is up.
//
// Call sites convert at point of use — see the COL_* macros in ui.cpp.

enum theme_mode_t {
    THEME_MODE_CLAUDE = 0,
    THEME_MODE_CODEX,
    THEME_MODE_COUNT,
};

struct Theme {
    uint32_t bg;        // screen background
    uint32_t panel;     // card/zone fill
    uint32_t text;      // primary text
    uint32_t dim;       // secondary text
    uint32_t accent;    // brand color
    uint32_t green;
    uint32_t amber;
    uint32_t red;
    uint32_t bar_bg;    // unfilled bar track

    // Bar color for a scoped-model face on the flipping Weekly card. Not a
    // brand color -- it marks "this face is one model's slice, not the whole
    // plan" -- so both themes use the same blue, which is the one Claude's own
    // usage settings use for the Fable allowance.
    uint32_t scoped;

    const char* name;   // shown on the usage screen

    // Claude's identity is a serif display face over sans body copy; ChatGPT's
    // is sans throughout. Set here rather than duplicating every breakpoint's
    // font table -- ui.cpp swaps the serif slots for their nearest sans size
    // after the layout has picked sizes.
    bool sans_only;

    // Fill for a progress bar below the warning threshold. Claude's identity
    // reads a healthy bar as green; the Codex language treats usage as neutral
    // information until it is actionable, so its normal fill is off-white and
    // only the warning/critical states carry color.
    uint32_t progress;

    // Quiet status line: a static dot and plain state words, instead of the
    // ping-pong spinner glyphs and rotating gerunds. Those are Claude Code's
    // own idiom -- showing them on a Codex screen puts Anthropic's product
    // voice on the wrong provider.
    bool quiet_status;
};

// The palettes differ in temperature as much as in hue, which is what makes
// them readable apart at arm's length: Claude's greys are warm and its accent
// is terra-cotta; the Codex greys are neutral and its accent is green.
static const Theme THEMES[THEME_MODE_COUNT] = {
    // Claude — Anthropic-inspired, AMOLED-friendly (true black bg).
    {
        .bg     = 0x000000,
        .panel  = 0x1f1f1e,
        .text   = 0xfaf9f5,
        .dim    = 0xb0aea5,
        .accent = 0xd97757,   // brand terra-cotta
        .green  = 0x788c5d,
        .amber  = 0xd97757,
        .red    = 0xc0392b,
        .bar_bg = 0x2a2a28,
        .scoped = 0x4a7dea,
        .name   = "Claude",
        .sans_only = false,
        .progress = 0x788c5d,        // healthy = green, as it always has been
        .quiet_status = false,
    },
    // Codex — ChatGPT-inspired: neutral greys, signature green.
    {
        .bg     = 0x000000,
        .panel  = 0x1a1a1a,
        .text   = 0xececec,
        .dim    = 0x9b9b9b,
        .accent = 0x10a37f,   // ChatGPT green
        .green  = 0x10a37f,
        .amber  = 0xe8a33d,
        .red    = 0xef4146,
        .bar_bg = 0x2a2a2a,
        .scoped = 0x4a7dea,
        .name   = "Codex",
        .sans_only = true,
        .progress = 0xe7e7e7,        // neutral until actionable
        .quiet_status = true,
    },
};

// Load the saved mode from NVS. Must run BEFORE ui_init() and splash_init():
// the mode picks the palette, the font family (via compute_layout) and the art
// set, all of which are read while those build their widgets.
void theme_init(void);

// Active palette. Mode is runtime state so the button handler can cycle it
// without a rebuild; theme_set_mode() persists the choice.
const Theme& theme(void);
theme_mode_t theme_mode(void);
void theme_set_mode(theme_mode_t mode);
theme_mode_t theme_next_mode(void);   // cycles, wrapping at THEME_MODE_COUNT

// Auto-flip: cycle the provider on a timer instead of only on a button press.
// Persisted next to the mode, since both answer "what is the screen showing"
// and both should survive a reboot.
//
// Index 0 is off, so a fresh device does nothing until asked, and one press
// past the slowest cadence returns to off.
#define AUTOFLIP_COUNT 5
static const uint32_t AUTOFLIP_MS[AUTOFLIP_COUNT] = { 0, 30000, 60000, 120000, 180000 };
static const char* const AUTOFLIP_LABEL[AUTOFLIP_COUNT] = { "Auto off", "Auto 30s", "Auto 1m", "Auto 2m", "Auto 3m" };

uint8_t  theme_autoflip_idx(void);   // 0 = off
uint32_t theme_autoflip_ms(void);    // 0 = off
const char* theme_autoflip_label(void);
void theme_cycle_autoflip(void);     // advances, wrapping through off
