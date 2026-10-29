//! The device's colour themes: the default Clawd palette and well-known
//! editor schemes mapped onto its eleven colours (the `theme` key; `custom`
//! uses the `color_*` keys instead).

use crate::config::Palette;

/// A named palette on the Device page.
#[derive(Debug, PartialEq, Eq)]
pub struct ThemePreset {
    /// The config value, `theme = <id>`.
    pub id: &'static str,
    /// The Device page's name for it.
    pub name: &'static str,
    /// Its colours.
    pub colors: Palette,
}

/// Every preset, the default first. Each lists background, cards, text,
/// secondary text, accent, low/medium/high usage, bar track, Clawd and the
/// face's eyes (the `th` order).
pub static PRESETS: [ThemePreset; 10] = [
    preset(
        "clawd",
        "Clawd",
        [0x000000, 0x1f1f1e, 0xfaf9f5, 0xb0aea5, 0xd97757, 0x788c5d, 0xd97757, 0xc0392b, 0x2a2a28, 0xd97757, 0x141413],
    ),
    preset(
        "catppuccin_mocha",
        "Catppuccin Mocha",
        [0x1e1e2e, 0x313244, 0xcdd6f4, 0xa6adc8, 0xcba6f7, 0xa6e3a1, 0xf9e2af, 0xf38ba8, 0x45475a, 0xfab387, 0x11111b],
    ),
    preset(
        "dracula",
        "Dracula",
        [0x21222c, 0x282a36, 0xf8f8f2, 0xa7abbe, 0xbd93f9, 0x50fa7b, 0xffb86c, 0xff5555, 0x44475a, 0xffb86c, 0x21222c],
    ),
    preset(
        "gruvbox",
        "Gruvbox",
        [0x1d2021, 0x282828, 0xebdbb2, 0xa89984, 0x83a598, 0xb8bb26, 0xfabd2f, 0xfb4934, 0x3c3836, 0xfe8019, 0x1d2021],
    ),
    preset(
        "monokai",
        "Monokai",
        [0x1e1f1c, 0x272822, 0xf8f8f2, 0xa59f85, 0x66d9ef, 0xa6e22e, 0xe6db74, 0xf92672, 0x3e3d32, 0xfd971f, 0x1e1f1c],
    ),
    preset(
        "nord",
        "Nord",
        [0x2e3440, 0x3b4252, 0xeceff4, 0xb4bccb, 0x88c0d0, 0xa3be8c, 0xebcb8b, 0xbf616a, 0x4c566a, 0xd08770, 0x2e3440],
    ),
    preset(
        "one_dark",
        "One Dark",
        [0x21252b, 0x282c34, 0xdcdfe4, 0xabb2bf, 0x61afef, 0x98c379, 0xe5c07b, 0xe06c75, 0x3e4451, 0xd19a66, 0x21252b],
    ),
    preset(
        "solarized_dark",
        "Solarized Dark",
        [0x002b36, 0x073642, 0xeee8d5, 0x93a1a1, 0x268bd2, 0x859900, 0xb58900, 0xdc322f, 0x002b36, 0xcb4b16, 0x002b36],
    ),
    preset(
        "solarized_light",
        "Solarized Light",
        [0xfdf6e3, 0xeee8d5, 0x073642, 0x586e75, 0x268bd2, 0x859900, 0xb58900, 0xdc322f, 0xfdf6e3, 0xcb4b16, 0x002b36],
    ),
    preset(
        "tokyo_night",
        "Tokyo Night",
        [0x16161e, 0x1a1b26, 0xc0caf5, 0xa9b1d6, 0x7aa2f7, 0x9ece6a, 0xe0af68, 0xf7768e, 0x292e42, 0xff9e64, 0x16161e],
    ),
];

const fn preset(id: &'static str, name: &'static str, colors: [u32; 11]) -> ThemePreset {
    ThemePreset { id, name, colors: Palette::new(colors) }
}

/// The default theme (the firmware's own palette).
pub fn default_preset() -> &'static ThemePreset {
    &PRESETS[0]
}

/// The preset named by a `theme` value (case-insensitive), if any.
pub fn find(id: &str) -> Option<&'static ThemePreset> {
    let id = id.trim();
    PRESETS.iter().find(|p| p.id.eq_ignore_ascii_case(id))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_default_is_the_firmware_palette() {
        assert_eq!(default_preset().colors, Palette::default());
        assert_eq!(find("Dracula").map(|p| p.name), Some("Dracula"));
        assert!(find("custom").is_none());
    }

    #[test]
    fn ids_and_names_are_unique() {
        for (i, a) in PRESETS.iter().enumerate() {
            for b in &PRESETS[i + 1..] {
                assert_ne!(a.id, b.id);
                assert_ne!(a.name, b.name);
                assert_ne!(a.colors, b.colors);
            }
            assert!(a.id.bytes().all(|c| c.is_ascii_lowercase() || c == b'_'), "{}", a.id);
        }
    }
}
