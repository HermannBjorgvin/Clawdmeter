//! The app's library half: the daemon runtime, the tray process, and the
//! GPUI windows, shared by the headless `--daemon` mode and the desktop app.
//!
//! Process layout: `clawdmeter` (no arguments) is the tray process. It runs
//! the daemon and the tray icon and lives for the whole session. The app
//! window is one separate `clawdmeter --window [page]` process running GPUI,
//! which exits when the window closes; the tray switches it between pages
//! instead of opening more windows (see [`ipc`] for why and how they talk).

#![warn(missing_docs)]

pub mod art;
pub mod boards;
pub mod chime;
pub mod daemon;
pub mod diff;
pub mod errors;
pub mod hooks;
pub mod ipc;
pub mod logger;
pub mod onboarding;
pub mod tray;
pub mod ui;

/// The app's version: `CLAWDMETER_VERSION` at build time (CI sets it from
/// the release tag; a leading `v` is dropped), else the crate version.
pub fn version() -> &'static str {
    const RAW: &str = match option_env!("CLAWDMETER_VERSION") {
        Some(v) => v,
        None => env!("CARGO_PKG_VERSION"),
    };
    let v = RAW.trim();
    let v = v.strip_prefix('v').unwrap_or(v);
    if v.is_empty() {
        env!("CARGO_PKG_VERSION")
    } else {
        v
    }
}

/// The pages of the app window. The window is one process and one window;
/// the tray (or a second launch) switches it to a page.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Page {
    /// Link state, live meters, refresh and mute.
    Overview,
    /// The board: firmware, display and sound options, forget.
    Device,
    /// The flasher.
    Flash,
    /// Claude Code config folders.
    Accounts,
    /// Claude Code hooks.
    Hooks,
    /// Start at login, polling, files, quit.
    Settings,
    /// The first-run setup wizard.
    Setup,
}

impl Page {
    /// Pages in sidebar order (setup is reached from the sidebar footer).
    pub const SIDEBAR: [Page; 6] =
        [Page::Overview, Page::Device, Page::Flash, Page::Accounts, Page::Hooks, Page::Settings];

    /// Parse a page name. Older window and tab names still work:
    /// `status` is the overview, `alerts` the device page, `flasher` the
    /// flash page, `background` the settings page.
    pub fn parse(s: &str) -> Option<Self> {
        Some(match s.to_ascii_lowercase().as_str() {
            "overview" | "status" => Page::Overview,
            "device" | "alerts" => Page::Device,
            "flash" | "flasher" => Page::Flash,
            "accounts" => Page::Accounts,
            "hooks" => Page::Hooks,
            "settings" | "background" => Page::Settings,
            "setup" => Page::Setup,
            _ => return None,
        })
    }

    /// The page's name (what [`Page::parse`] reads back).
    pub fn as_str(self) -> &'static str {
        match self {
            Page::Overview => "overview",
            Page::Device => "device",
            Page::Flash => "flash",
            Page::Accounts => "accounts",
            Page::Hooks => "hooks",
            Page::Settings => "settings",
            Page::Setup => "setup",
        }
    }

    /// The sidebar label.
    pub fn label(self) -> &'static str {
        match self {
            Page::Overview => "Overview",
            Page::Device => "Device",
            Page::Flash => "Flash",
            Page::Accounts => "Accounts",
            Page::Hooks => "Hooks",
            Page::Settings => "Settings",
            Page::Setup => "Setup",
        }
    }
}

/// Shorten a path under the home dir to `~/…` for display.
pub fn tilde(path: &str, home: &std::path::Path) -> String {
    let home = home.to_string_lossy();
    match path.strip_prefix(home.as_ref()) {
        Some("") => "~".to_owned(),
        Some(rest) if rest.starts_with('/') || rest.starts_with('\\') => format!("~{rest}"),
        _ => path.to_owned(),
    }
}

/// Which Claude Code reads usage, for display: the desktop app's own copy
/// (with its version) or the executable's path.
pub fn claude_label(claude: &std::path::Path, home: &std::path::Path) -> String {
    use clawdmeter_core::usage::{desktop_claude_root, is_desktop_bundled};
    if is_desktop_bundled(claude, home) {
        let version = desktop_claude_root(home)
            .and_then(|r| claude.strip_prefix(r).ok().and_then(|rest| rest.iter().next()).map(|v| v.to_owned()));
        return match version {
            Some(v) => format!("the Claude desktop app's Claude Code {}", v.to_string_lossy()),
            None => "the Claude desktop app's Claude Code".into(),
        };
    }
    tilde(&claude.display().to_string(), home)
}

/// "12 s ago" / "3 min ago" / "2 h ago".
pub fn ago(secs: i64) -> String {
    match secs.max(0) {
        s if s < 60 => format!("{s} s ago"),
        s if s < 3600 => format!("{} min ago", s / 60),
        s => format!("{} h ago", s / 3600),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn claude_labels() {
        let home = std::path::Path::new("/home/me");
        assert_eq!(claude_label(std::path::Path::new("/home/me/.local/bin/claude"), home), "~/.local/bin/claude");
        if let Some(root) = clawdmeter_core::usage::desktop_claude_root(home) {
            assert_eq!(
                claude_label(&root.join("2.1.295").join("71164c85f9d2").join("claude"), home),
                "the Claude desktop app's Claude Code 2.1.295"
            );
        }
    }

    #[test]
    fn tilde_and_ago() {
        let home = std::path::Path::new("/home/u");
        assert_eq!(tilde("/home/u/.claude", home), "~/.claude");
        assert_eq!(tilde("/home/u", home), "~");
        assert_eq!(tilde("/home/uu/.claude", home), "/home/uu/.claude");
        assert_eq!(ago(12), "12 s ago");
        assert_eq!(ago(200), "3 min ago");
        assert_eq!(ago(7300), "2 h ago");
        for p in Page::SIDEBAR.into_iter().chain([Page::Setup]) {
            assert_eq!(Page::parse(p.as_str()), Some(p));
        }
        assert_eq!(Page::parse("background"), Some(Page::Settings));
        assert_eq!(Page::parse("alerts"), Some(Page::Device));
        assert_eq!(Page::parse("flasher"), Some(Page::Flash));
        assert_eq!(Page::parse("status"), Some(Page::Overview));
        assert_eq!(Page::parse("nowhere"), None);
    }
}
