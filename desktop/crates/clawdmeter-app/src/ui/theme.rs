//! Design tokens. The neutrals are the desktop's own, quiet greys in the
//! family of the Claude desktop app; the accent and the usage colours are
//! the device's (`THEME_*` in `firmware/src/theme.h`). Clawd orange is for
//! Clawd and primary actions only. Light and dark follow the OS appearance.

use gpui::{rgb, Rgba, WindowAppearance};

/// Display face (Tiempos Text).
pub const FONT_DISPLAY: &str = "Tiempos Text";
/// Body face (Styrene B).
pub const FONT_BODY: &str = "Styrene B";
/// Data face (DejaVu Sans Mono).
pub const FONT_MONO: &str = "DejaVu Sans Mono";

/// Radius for inputs.
pub const R_S: f32 = 6.0;
/// Radius for buttons and list rows (sidebar items).
pub const R_ITEM: f32 = 8.0;
/// Radius for panels (the device's zone radius).
pub const R_M: f32 = 12.0;
/// Pill radius.
pub const R_PILL: f32 = 999.0;

/// Resolved colour tokens.
#[derive(Debug, Clone, Copy)]
pub struct Theme {
    /// Sidebar, title bar and inset wells (logs, diffs).
    pub bg: Rgba,
    /// The page background.
    pub surface: Rgba,
    /// Cards.
    pub panel: Rgba,
    /// Row hover.
    pub hover: Rgba,
    /// Selected row, quiet button fill.
    pub selected: Rgba,
    /// Hairlines.
    pub line: Rgba,
    /// Bar and switch tracks.
    pub track: Rgba,
    /// Body text.
    pub text: Rgba,
    /// Secondary text, icons.
    pub dim: Rgba,
    /// Tertiary text (help, captions).
    pub mute: Rgba,
    /// `--accent`: THEME_ACCENT, Clawd and primary actions.
    pub accent: Rgba,
    /// `--accent-ink`: text on accent.
    pub accent_ink: Rgba,
    /// `--ok`: THEME_GREEN.
    pub ok: Rgba,
    /// `--warn`: desktop amber (the firmware's THEME_AMBER is the accent).
    pub warn: Rgba,
    /// `--bad`: THEME_RED family.
    pub bad: Rgba,
    /// `--focus`.
    pub focus: Rgba,
    /// Dark appearance.
    pub dark: bool,
}

impl Theme {
    /// The dark tokens.
    pub fn dark() -> Self {
        Theme {
            bg: rgb(0x111111),
            surface: rgb(0x171717),
            panel: rgb(0x1f1f1f),
            hover: rgb(0x232323),
            selected: rgb(0x2e2e2e),
            line: rgb(0x2a2a2a),
            track: rgb(0x2c2c2c),
            text: rgb(0xececec),
            dim: rgb(0xb4b4b4),
            mute: rgb(0x8a8a8a),
            accent: rgb(0xd97757),
            accent_ink: rgb(0x141413),
            ok: rgb(0x788c5d),
            warn: rgb(0xd4a24c),
            bad: rgb(0xd4553f),
            focus: rgb(0xe8a48b),
            dark: true,
        }
    }

    /// The light tokens.
    pub fn light() -> Self {
        Theme {
            bg: rgb(0xf4f4f3),
            surface: rgb(0xfcfcfc),
            panel: rgb(0xf2f2f1),
            hover: rgb(0xe9e9e8),
            selected: rgb(0xe1e1df),
            line: rgb(0xe3e3e1),
            track: rgb(0xe4e4e2),
            text: rgb(0x1a1a1a),
            dim: rgb(0x525252),
            mute: rgb(0x737373),
            accent: rgb(0xc6613f),
            accent_ink: rgb(0xffffff),
            ok: rgb(0x5f7348),
            warn: rgb(0xa87a1f),
            bad: rgb(0xb33a26),
            focus: rgb(0xc6613f),
            dark: false,
        }
    }

    /// Follow the window's appearance.
    pub fn for_appearance(a: WindowAppearance) -> Self {
        match a {
            WindowAppearance::Light | WindowAppearance::VibrantLight => Theme::light(),
            WindowAppearance::Dark | WindowAppearance::VibrantDark => Theme::dark(),
        }
    }
}
