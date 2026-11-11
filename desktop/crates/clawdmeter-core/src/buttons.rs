//! What the device's buttons do: the config values (`button_primary`,
//! `button_secondary`, `button_pwr`), their labels, the payload codes `k1` /
//! `k2` / `k3` (`firmware/src/button_action.h`) and the mapping from a
//! recorded keystroke to a HID keyboard usage.
//!
//! The board types HID usages, which are key *positions*: the host applies
//! its own keyboard layout. Recorded keys are mapped as US-layout positions.

/// HID modifier bits, the keyboard report's modifier byte (left-hand keys).
pub const MOD_CTRL: u8 = 0x01;
/// See [`MOD_CTRL`].
pub const MOD_SHIFT: u8 = 0x02;
/// See [`MOD_CTRL`].
pub const MOD_ALT: u8 = 0x04;
/// The GUI key: Cmd on macOS, Win on Windows, Super on Linux.
pub const MOD_GUI: u8 = 0x08;

const KEY_FLAG: u32 = 0x1_0000;
const CONSUMER_FLAG: u32 = 0x2_0000;
const HOLD_BIT: u32 = 0x1000;

/// A physical button the app can remap.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Button {
    /// BOOT on most boards (`k1`).
    Primary,
    /// The second button, where fitted (`k2`).
    Secondary,
    /// A PWR short press (`k3`); its hold to pair is fixed.
    Pwr,
}

impl Button {
    /// Every button, in payload order.
    pub const ALL: [Button; 3] = [Button::Primary, Button::Secondary, Button::Pwr];

    /// Its index in [`crate::Settings::buttons`].
    pub fn index(self) -> usize {
        self as usize
    }

    /// The config key.
    pub fn config_key(self) -> &'static str {
        match self {
            Button::Primary => "button_primary",
            Button::Secondary => "button_secondary",
            Button::Pwr => "button_pwr",
        }
    }

    /// What it does out of the box.
    pub fn default_action(self) -> ButtonAction {
        match self {
            Button::Primary => ButtonAction::Space,
            Button::Secondary => ButtonAction::ShiftTab,
            Button::Pwr => ButtonAction::Brightness,
        }
    }

    /// Whether the board sees it released, so keys can stay down while it's
    /// held. A PWR short press only registers on release.
    pub fn can_hold(self) -> bool {
        self != Button::Pwr
    }
}

/// A media key: a HID Consumer Control usage.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MediaKey {
    /// Play/Pause.
    PlayPause,
    /// Previous track.
    Previous,
    /// Next track.
    Next,
    /// Volume down (repeats while the button is held).
    VolumeDown,
    /// Volume up (repeats while the button is held).
    VolumeUp,
    /// Mute.
    Mute,
}

impl MediaKey {
    /// Every media key, in the order the Device page shows them.
    pub const ALL: [MediaKey; 6] = [
        MediaKey::PlayPause,
        MediaKey::Previous,
        MediaKey::Next,
        MediaKey::VolumeDown,
        MediaKey::VolumeUp,
        MediaKey::Mute,
    ];

    /// The HID Consumer usage.
    pub fn usage(self) -> u16 {
        match self {
            MediaKey::PlayPause => 0xCD,
            MediaKey::Previous => 0xB6,
            MediaKey::Next => 0xB5,
            MediaKey::VolumeDown => 0xEA,
            MediaKey::VolumeUp => 0xE9,
            MediaKey::Mute => 0xE2,
        }
    }

    /// The config spelling.
    pub fn as_str(self) -> &'static str {
        match self {
            MediaKey::PlayPause => "play-pause",
            MediaKey::Previous => "previous-track",
            MediaKey::Next => "next-track",
            MediaKey::VolumeDown => "volume-down",
            MediaKey::VolumeUp => "volume-up",
            MediaKey::Mute => "mute",
        }
    }

    /// The Device page's short label.
    pub fn label(self) -> &'static str {
        match self {
            MediaKey::PlayPause => "Play/Pause",
            MediaKey::Previous => "Previous",
            MediaKey::Next => "Next",
            MediaKey::VolumeDown => "Vol −",
            MediaKey::VolumeUp => "Vol +",
            MediaKey::Mute => "Mute",
        }
    }

    fn parse(v: &str) -> Option<Self> {
        Some(match v {
            "play-pause" | "play" | "pause" => MediaKey::PlayPause,
            "previous-track" | "previous" | "prev" => MediaKey::Previous,
            "next-track" | "next" => MediaKey::Next,
            "volume-down" | "vol-down" => MediaKey::VolumeDown,
            "volume-up" | "vol-up" => MediaKey::VolumeUp,
            "mute" => MediaKey::Mute,
            _ => return None,
        })
    }
}

/// A keyboard shortcut: one key and any of Ctrl, Shift, Alt and GUI.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct KeyChord {
    /// [`MOD_CTRL`] | [`MOD_SHIFT`] | [`MOD_ALT`] | [`MOD_GUI`].
    pub mods: u8,
    /// The HID keyboard usage (one of [`KEYS`]).
    pub usage: u8,
    /// Keep the keys down while the button is held (push to talk); else tap.
    pub hold: bool,
}

/// One key the board can type.
struct Key {
    usage: u8,
    /// Config spelling (and GPUI's name for it where it has one).
    name: &'static str,
    /// Other spellings: GPUI's symbol for punctuation, short forms.
    aliases: &'static [&'static str],
    label: &'static str,
    mac_label: &'static str,
}

const fn k(usage: u8, name: &'static str, label: &'static str) -> Key {
    Key { usage, name, aliases: &[], label, mac_label: label }
}

const fn ka(usage: u8, name: &'static str, aliases: &'static [&'static str], label: &'static str) -> Key {
    Key { usage, name, aliases, label, mac_label: label }
}

const fn km(
    usage: u8,
    name: &'static str,
    aliases: &'static [&'static str],
    label: &'static str,
    mac: &'static str,
) -> Key {
    Key { usage, name, aliases, label, mac_label: mac }
}

/// Every key a shortcut can use, by HID usage (US positions). Not the keypad:
/// GPUI reports its keys like the main ones on macOS and Windows.
static KEYS: &[Key] = &[
    k(0x04, "a", "A"),
    k(0x05, "b", "B"),
    k(0x06, "c", "C"),
    k(0x07, "d", "D"),
    k(0x08, "e", "E"),
    k(0x09, "f", "F"),
    k(0x0A, "g", "G"),
    k(0x0B, "h", "H"),
    k(0x0C, "i", "I"),
    k(0x0D, "j", "J"),
    k(0x0E, "k", "K"),
    k(0x0F, "l", "L"),
    k(0x10, "m", "M"),
    k(0x11, "n", "N"),
    k(0x12, "o", "O"),
    k(0x13, "p", "P"),
    k(0x14, "q", "Q"),
    k(0x15, "r", "R"),
    k(0x16, "s", "S"),
    k(0x17, "t", "T"),
    k(0x18, "u", "U"),
    k(0x19, "v", "V"),
    k(0x1A, "w", "W"),
    k(0x1B, "x", "X"),
    k(0x1C, "y", "Y"),
    k(0x1D, "z", "Z"),
    k(0x1E, "1", "1"),
    k(0x1F, "2", "2"),
    k(0x20, "3", "3"),
    k(0x21, "4", "4"),
    k(0x22, "5", "5"),
    k(0x23, "6", "6"),
    k(0x24, "7", "7"),
    k(0x25, "8", "8"),
    k(0x26, "9", "9"),
    k(0x27, "0", "0"),
    km(0x28, "enter", &["return"], "Enter", "↩"),
    km(0x29, "escape", &["esc"], "Esc", "⎋"),
    km(0x2A, "backspace", &[], "Backspace", "⌫"),
    km(0x2B, "tab", &[], "Tab", "⇥"),
    k(0x2C, "space", "Space"),
    ka(0x2D, "minus", &["-"], "-"),
    ka(0x2E, "equal", &["="], "="),
    ka(0x2F, "bracketleft", &["["], "["),
    ka(0x30, "bracketright", &["]"], "]"),
    ka(0x31, "backslash", &["\\"], "\\"),
    ka(0x33, "semicolon", &[";"], ";"),
    ka(0x34, "quote", &["'"], "'"),
    ka(0x35, "grave", &["`"], "`"),
    ka(0x36, "comma", &[","], ","),
    ka(0x37, "period", &["."], "."),
    ka(0x38, "slash", &["/"], "/"),
    k(0x3A, "f1", "F1"),
    k(0x3B, "f2", "F2"),
    k(0x3C, "f3", "F3"),
    k(0x3D, "f4", "F4"),
    k(0x3E, "f5", "F5"),
    k(0x3F, "f6", "F6"),
    k(0x40, "f7", "F7"),
    k(0x41, "f8", "F8"),
    k(0x42, "f9", "F9"),
    k(0x43, "f10", "F10"),
    k(0x44, "f11", "F11"),
    k(0x45, "f12", "F12"),
    km(0x49, "insert", &["ins"], "Insert", "Insert"),
    km(0x4A, "home", &[], "Home", "↖"),
    km(0x4B, "pageup", &["pgup"], "PgUp", "⇞"),
    km(0x4C, "delete", &["del"], "Delete", "⌦"),
    km(0x4D, "end", &[], "End", "↘"),
    km(0x4E, "pagedown", &["pgdn"], "PgDn", "⇟"),
    km(0x4F, "right", &[], "Right", "→"),
    km(0x50, "left", &[], "Left", "←"),
    km(0x51, "down", &[], "Down", "↓"),
    km(0x52, "up", &[], "Up", "↑"),
    k(0x65, "menu", "Menu"),
    k(0x68, "f13", "F13"),
    k(0x69, "f14", "F14"),
    k(0x6A, "f15", "F15"),
    k(0x6B, "f16", "F16"),
    k(0x6C, "f17", "F17"),
    k(0x6D, "f18", "F18"),
    k(0x6E, "f19", "F19"),
    k(0x6F, "f20", "F20"),
    k(0x70, "f21", "F21"),
    k(0x71, "f22", "F22"),
    k(0x72, "f23", "F23"),
    k(0x73, "f24", "F24"),
];

/// GPUI reports a shifted digit or punctuation key as the character it types
/// (and drops Shift): on a US layout, the key it's on.
const SHIFTED: &[(&str, &str)] = &[
    ("!", "1"),
    ("@", "2"),
    ("#", "3"),
    ("$", "4"),
    ("%", "5"),
    ("^", "6"),
    ("&", "7"),
    ("*", "8"),
    ("(", "9"),
    (")", "0"),
    ("_", "-"),
    ("+", "="),
    ("{", "["),
    ("}", "]"),
    ("|", "\\"),
    (":", ";"),
    ("\"", "'"),
    ("~", "`"),
    ("<", ","),
    (">", "."),
    ("?", "/"),
];

fn key_by_name(name: &str) -> Option<&'static Key> {
    KEYS.iter().find(|k| k.name == name || k.aliases.contains(&name))
}

fn key_by_usage(usage: u8) -> Option<&'static Key> {
    KEYS.iter().find(|k| k.usage == usage)
}

/// Whose conventions labels follow.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LabelStyle {
    /// ⌃⌥⇧⌘ symbols, no separators.
    Mac,
    /// Win+Ctrl+Alt+Shift+Key.
    Windows,
    /// Super+Ctrl+Alt+Shift+Key.
    Linux,
}

impl LabelStyle {
    /// This computer's.
    pub fn current() -> Self {
        if cfg!(target_os = "macos") {
            LabelStyle::Mac
        } else if cfg!(windows) {
            LabelStyle::Windows
        } else {
            LabelStyle::Linux
        }
    }

    /// The GUI modifier's name.
    pub fn gui(self) -> &'static str {
        match self {
            LabelStyle::Mac => "⌘",
            LabelStyle::Windows => "Win",
            LabelStyle::Linux => "Super",
        }
    }

    /// The modifiers in `mods` as they lead a label: `Ctrl+Shift+`, `⇧⌘`;
    /// empty for none.
    pub fn prefix(self, mods: u8) -> String {
        let order: &[u8] = match self {
            LabelStyle::Mac => &[MOD_CTRL, MOD_ALT, MOD_SHIFT, MOD_GUI],
            _ => &[MOD_GUI, MOD_CTRL, MOD_ALT, MOD_SHIFT],
        };
        let names = order.iter().filter(|b| mods & **b != 0).map(|b| self.modifier(*b));
        if self == LabelStyle::Mac {
            names.collect()
        } else {
            names.map(|n| format!("{n}+")).collect()
        }
    }

    /// A modifier's name ([`MOD_CTRL`] etc.).
    pub fn modifier(self, bit: u8) -> &'static str {
        match (self, bit) {
            (_, MOD_GUI) => self.gui(),
            (LabelStyle::Mac, MOD_CTRL) => "⌃",
            (LabelStyle::Mac, MOD_ALT) => "⌥",
            (LabelStyle::Mac, MOD_SHIFT) => "⇧",
            (_, MOD_CTRL) => "Ctrl",
            (_, MOD_ALT) => "Alt",
            _ => "Shift",
        }
    }
}

/// Why a recorded key can't be used.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Unmappable(pub String);

impl KeyChord {
    /// The chord a recorded keystroke stands for. `key` is GPUI's
    /// `Keystroke::key`: a lower-case letter, a digit, a name such as `f13`
    /// or `pageup`, or the character a punctuation key typed (shifted ones
    /// arrive without Shift).
    pub fn from_keystroke(key: &str, ctrl: bool, alt: bool, shift: bool, platform: bool) -> Result<Self, Unmappable> {
        let mut mods = 0;
        for (on, bit) in [(ctrl, MOD_CTRL), (shift, MOD_SHIFT), (alt, MOD_ALT), (platform, MOD_GUI)] {
            if on {
                mods |= bit;
            }
        }
        let lower = key.to_lowercase();
        let found = match SHIFTED.iter().find(|(s, _)| *s == key) {
            Some((_, base)) => {
                mods |= MOD_SHIFT;
                key_by_name(base)
            }
            None => key_by_name(&lower),
        };
        match found {
            Some(k) => Ok(KeyChord { mods, usage: k.usage, hold: false }),
            None => Err(Unmappable(unmappable_message(key))),
        }
    }

    /// The label in `style`'s conventions, e.g. Ctrl+Shift+M or ⇧⌘M.
    pub fn label(self, style: LabelStyle) -> String {
        let key =
            key_by_usage(self.usage).map_or("?", |k| if style == LabelStyle::Mac { k.mac_label } else { k.label });
        format!("{}{key}", style.prefix(self.mods))
    }

    /// The config spelling without the hold/tap suffix: `ctrl+shift+m`.
    fn combo(self) -> String {
        let mut parts: Vec<&str> = [(MOD_CTRL, "ctrl"), (MOD_SHIFT, "shift"), (MOD_ALT, "alt"), (MOD_GUI, "super")]
            .iter()
            .filter(|(b, _)| self.mods & b != 0)
            .map(|(_, n)| *n)
            .collect();
        parts.push(key_by_usage(self.usage).map_or("?", |k| k.name));
        parts.join("+")
    }

    fn parse(combo: &str, hold: bool) -> Option<Self> {
        let parts: Vec<&str> = combo.split('+').collect();
        let (key, mods) = parts.split_last()?;
        let mut bits = 0;
        for m in mods {
            bits |= match *m {
                "ctrl" | "control" => MOD_CTRL,
                "shift" => MOD_SHIFT,
                "alt" | "option" | "opt" => MOD_ALT,
                "super" | "cmd" | "command" | "win" | "gui" | "meta" | "logo" => MOD_GUI,
                _ => return None,
            };
        }
        Some(KeyChord { mods: bits, usage: key_by_name(key)?.usage, hold })
    }
}

fn unmappable_message(key: &str) -> String {
    let shown = if key.chars().count() == 1 { key.to_uppercase() } else { key.to_owned() };
    if key.chars().count() == 1 && !key.is_ascii() {
        format!("{shown} isn't on a US keyboard, and the board types US key positions. Try a letter, digit or F-key.")
    } else {
        format!("The board can't send {shown}. Try a letter, digit, F-key, arrow or another editing key.")
    }
}

/// What a device button does.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ButtonAction {
    /// Nothing (a press still wakes the screen and dismisses an alert).
    None,
    /// Space, held while the button is: Claude Code's voice push to talk.
    Space,
    /// Shift+Tab: Claude Code's mode toggle.
    ShiftTab,
    /// Tab.
    Tab,
    /// Enter.
    Enter,
    /// Escape.
    Escape,
    /// Cycle the screen brightness.
    Brightness,
    /// A media key.
    Media(MediaKey),
    /// A recorded keyboard shortcut.
    Chord(KeyChord),
}

impl ButtonAction {
    /// The single keys the Device page offers, in its order.
    pub const KEYS: [ButtonAction; 5] =
        [ButtonAction::Space, ButtonAction::ShiftTab, ButtonAction::Tab, ButtonAction::Enter, ButtonAction::Escape];

    /// Parse a config value; anything unknown is `default`. A shortcut is
    /// modifiers and a key joined by `+`, then `hold` (keys stay down while
    /// the button is held) or `tap` (the default; written only where the bare
    /// form would read as a preset, `space tap`).
    pub fn parse(v: &str, default: ButtonAction) -> Self {
        let v = v.trim().to_ascii_lowercase();
        let mut words = v.split_whitespace();
        let (Some(combo), suffix, None) = (words.next(), words.next(), words.next()) else { return default };
        let hold = match suffix {
            None => {
                if let Some(a) = Self::preset(combo) {
                    return a;
                }
                false
            }
            Some("hold") => true,
            Some("tap") => false,
            Some(_) => return default,
        };
        KeyChord::parse(combo, hold).map_or(default, ButtonAction::Chord)
    }

    fn preset(v: &str) -> Option<Self> {
        Some(match v {
            "none" | "off" | "nothing" => ButtonAction::None,
            "space" => ButtonAction::Space,
            "shift-tab" | "shift+tab" => ButtonAction::ShiftTab,
            "tab" => ButtonAction::Tab,
            "enter" => ButtonAction::Enter,
            "escape" | "esc" => ButtonAction::Escape,
            "brightness" => ButtonAction::Brightness,
            _ => ButtonAction::Media(MediaKey::parse(v)?),
        })
    }

    /// The config-file spelling.
    pub fn as_str(self) -> String {
        match self {
            ButtonAction::None => "none".into(),
            ButtonAction::Space => "space".into(),
            ButtonAction::ShiftTab => "shift-tab".into(),
            ButtonAction::Tab => "tab".into(),
            ButtonAction::Enter => "enter".into(),
            ButtonAction::Escape => "escape".into(),
            ButtonAction::Brightness => "brightness".into(),
            ButtonAction::Media(m) => m.as_str().into(),
            ButtonAction::Chord(c) => {
                let combo = c.combo();
                if c.hold {
                    format!("{combo} hold")
                } else if Self::preset(&combo).is_some() {
                    format!("{combo} tap")
                } else {
                    combo
                }
            }
        }
    }

    /// The Device page's label.
    pub fn label(self, style: LabelStyle) -> String {
        match self {
            ButtonAction::None => "Nothing".into(),
            ButtonAction::Space => "Space".into(),
            ButtonAction::ShiftTab => "Shift+Tab".into(),
            ButtonAction::Tab => "Tab".into(),
            ButtonAction::Enter => "Enter".into(),
            ButtonAction::Escape => "Esc".into(),
            ButtonAction::Brightness => "Brightness".into(),
            ButtonAction::Media(m) => m.label().into(),
            ButtonAction::Chord(c) => c.label(style),
        }
    }

    /// The payload's `k1` / `k2` / `k3` value (`firmware/src/button_action.h`):
    /// a preset 0–6; a shortcut `0x10000 | hold << 12 | mods << 8 | usage`;
    /// a media key `0x20000 | usage`.
    pub fn code(self) -> u32 {
        match self {
            ButtonAction::None => 0,
            ButtonAction::Space => 1,
            ButtonAction::ShiftTab => 2,
            ButtonAction::Tab => 3,
            ButtonAction::Enter => 4,
            ButtonAction::Escape => 5,
            ButtonAction::Brightness => 6,
            ButtonAction::Media(m) => CONSUMER_FLAG | u32::from(m.usage()),
            ButtonAction::Chord(c) => {
                KEY_FLAG | if c.hold { HOLD_BIT } else { 0 } | u32::from(c.mods & 0x0F) << 8 | u32::from(c.usage)
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn chord(mods: u8, usage: u8, hold: bool) -> ButtonAction {
        ButtonAction::Chord(KeyChord { mods, usage, hold })
    }

    #[test]
    fn codes_match_the_firmware() {
        let presets = [
            ButtonAction::None,
            ButtonAction::Space,
            ButtonAction::ShiftTab,
            ButtonAction::Tab,
            ButtonAction::Enter,
            ButtonAction::Escape,
            ButtonAction::Brightness,
        ];
        assert_eq!(presets.map(ButtonAction::code), [0, 1, 2, 3, 4, 5, 6], "never renumbered");
        // The values test_button_action and test_payload decode.
        assert_eq!(chord(MOD_CTRL | MOD_SHIFT, 0x10, false).code(), 0x10310, "Ctrl+Shift+M");
        assert_eq!(chord(MOD_CTRL, 0x2C, true).code(), 0x1112C, "Ctrl+Space, held");
        assert_eq!(chord(MOD_GUI | MOD_ALT, 0x0E, false).code(), 0x10C0E, "Win+Alt+K");
        assert_eq!(ButtonAction::Media(MediaKey::PlayPause).code(), 0x200CD);
        assert_eq!(ButtonAction::Media(MediaKey::VolumeUp).code(), 0x200E9);
        // Every key is inside the keyboard report's usage range (A..F24).
        assert!(KEYS.iter().all(|k| (0x04..=0x73).contains(&k.usage)));
        let mut usages: Vec<u8> = KEYS.iter().map(|k| k.usage).collect();
        usages.sort_unstable();
        usages.dedup();
        assert_eq!(usages.len(), KEYS.len(), "one entry per usage");
    }

    #[test]
    fn config_round_trip() {
        let all = [
            ButtonAction::None,
            ButtonAction::Space,
            ButtonAction::ShiftTab,
            ButtonAction::Tab,
            ButtonAction::Enter,
            ButtonAction::Escape,
            ButtonAction::Brightness,
            chord(MOD_CTRL | MOD_SHIFT, 0x10, false),
            chord(MOD_CTRL, 0x2C, true),
            chord(MOD_GUI | MOD_ALT, 0x0E, false),
            chord(0, 0x2C, false),         // Space tapped: not the held preset
            chord(MOD_SHIFT, 0x2B, false), // Shift+Tab as a shortcut
            chord(0, 0x2B, true),
            chord(0, 0x73, false),
            chord(MOD_CTRL, 0x2D, false),
        ];
        for a in all.into_iter().chain(MediaKey::ALL.map(ButtonAction::Media)) {
            assert_eq!(ButtonAction::parse(&a.as_str(), ButtonAction::None), a, "{}", a.as_str());
        }
        assert_eq!(chord(MOD_CTRL | MOD_SHIFT, 0x10, false).as_str(), "ctrl+shift+m");
        assert_eq!(chord(MOD_CTRL, 0x2C, true).as_str(), "ctrl+space hold");
        assert_eq!(chord(0, 0x2C, false).as_str(), "space tap");
        assert_eq!(chord(MOD_SHIFT, 0x2B, false).as_str(), "shift+tab tap");
        assert_eq!(chord(MOD_CTRL, 0x2D, false).as_str(), "ctrl+minus");
    }

    #[test]
    fn hand_written_values() {
        let d = ButtonAction::Brightness;
        assert_eq!(ButtonAction::parse("Win+Alt+K", d), chord(MOD_GUI | MOD_ALT, 0x0E, false));
        assert_eq!(ButtonAction::parse("cmd+shift+m", d), chord(MOD_GUI | MOD_SHIFT, 0x10, false));
        assert_eq!(ButtonAction::parse("  alt+a  ", d), chord(MOD_ALT, 0x04, false));
        assert_eq!(ButtonAction::parse("ctrl+-", d), chord(MOD_CTRL, 0x2D, false));
        assert_eq!(ButtonAction::parse("ctrl+space HOLD", d), chord(MOD_CTRL, 0x2C, true));
        assert_eq!(ButtonAction::parse("f13", d), chord(0, 0x68, false));
        assert_eq!(ButtonAction::parse("esc", d), ButtonAction::Escape);
        assert_eq!(ButtonAction::parse("next", d), ButtonAction::Media(MediaKey::Next));
        for bad in ["", "bogus", "ctrl+", "ctrl+shift", "hyper+a", "a hold tap", "a sometimes", "ctrl+f25", "+"] {
            assert_eq!(ButtonAction::parse(bad, d), d, "{bad:?}");
        }
    }

    #[test]
    fn keystrokes_map_to_us_positions() {
        let ks = |key: &str, ctrl, alt, shift, platform| KeyChord::from_keystroke(key, ctrl, alt, shift, platform);
        assert_eq!(
            ks("m", true, false, true, false),
            Ok(KeyChord { mods: MOD_CTRL | MOD_SHIFT, usage: 0x10, hold: false })
        );
        assert_eq!(ks("k", false, true, false, true).unwrap().mods, MOD_ALT | MOD_GUI);
        assert_eq!(ks("space", true, false, false, false).unwrap().usage, 0x2C);
        assert_eq!(ks("escape", false, false, false, false).unwrap().usage, 0x29);
        assert_eq!(ks("f24", false, false, false, false).unwrap().usage, 0x73);
        assert_eq!(ks("f1", false, false, false, false).unwrap().usage, 0x3A);
        assert_eq!(ks("pagedown", false, false, false, false).unwrap().usage, 0x4E);
        assert_eq!(ks("delete", false, false, false, false).unwrap().usage, 0x4C);
        assert_eq!(ks("/", false, false, false, false).unwrap().usage, 0x38);
        // Shifted keys arrive as what they type, without Shift.
        assert_eq!(
            ks("!", true, false, false, false),
            Ok(KeyChord { mods: MOD_CTRL | MOD_SHIFT, usage: 0x1E, hold: false })
        );
        assert_eq!(
            ks("?", false, false, false, false).unwrap(),
            KeyChord { mods: MOD_SHIFT, usage: 0x38, hold: false }
        );
        assert_eq!(ks("M", false, false, true, false).unwrap().usage, 0x10);
        for bad in ["ö", "f25", "capslock", "printscreen", "back", ""] {
            assert!(ks(bad, false, false, false, false).is_err(), "{bad:?}");
        }
        assert!(ks("ö", false, false, false, false).unwrap_err().0.contains("US"));
    }

    #[test]
    fn labels_follow_the_os() {
        let teams = KeyChord { mods: MOD_CTRL | MOD_SHIFT, usage: 0x10, hold: false };
        assert_eq!(teams.label(LabelStyle::Linux), "Ctrl+Shift+M");
        assert_eq!(teams.label(LabelStyle::Mac), "⌃⇧M");
        let mic = KeyChord { mods: MOD_GUI | MOD_ALT, usage: 0x0E, hold: false };
        assert_eq!(mic.label(LabelStyle::Windows), "Win+Alt+K");
        assert_eq!(mic.label(LabelStyle::Linux), "Super+Alt+K");
        assert_eq!(mic.label(LabelStyle::Mac), "⌥⌘K");
        assert_eq!(KeyChord { mods: 0, usage: 0x52, hold: false }.label(LabelStyle::Mac), "↑");
        assert_eq!(LabelStyle::Windows.prefix(MOD_CTRL | MOD_SHIFT), "Ctrl+Shift+");
        assert_eq!(LabelStyle::Mac.prefix(MOD_GUI | MOD_SHIFT), "⇧⌘");
        assert_eq!(LabelStyle::Linux.prefix(0), "");
        assert_eq!(ButtonAction::ShiftTab.label(LabelStyle::Mac), "Shift+Tab");
        assert_eq!(ButtonAction::Media(MediaKey::VolumeUp).label(LabelStyle::Mac), "Vol +");
    }
}
