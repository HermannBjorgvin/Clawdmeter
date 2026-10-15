//! How the windows see the daemon.
//!
//! The tray process owns the daemon. The app window is its own GPUI process
//! (`clawdmeter --window [page]`), because GPUI ends its event loop when the
//! last window closes on Linux and Windows, and a tray app has no window most
//! of the time. There is only ever one window process (`window.lock`); the
//! tray switches it between pages rather than opening another. The two sides
//! talk through small files in the state dir, the same way the hook and the
//! daemon already do:
//!
//! - `app-status.json`: the tray process rewrites it whenever the status
//!   changes (and at least every few seconds, so the window can tell a live
//!   daemon from a dead one by `updated_at`).
//! - `app-commands`: the window appends one command per line; the tray
//!   process takes the file (rename, read, delete) every tick.
//! - `window-commands`: `show <page>` lines for the window process, taken the
//!   same way: bring the window to the front on that page.

use std::fs;
use std::io::{self, Write as _};
use std::path::PathBuf;

use clawdmeter_core::Paths;
use serde::{Deserialize, Serialize};

use crate::art::TrayState;
use crate::daemon::{LinkState, Status};

/// `DATA_FRESH_MS` in the firmware: no write for this long reads as stale.
pub const DATA_FRESH_SECS: i64 = 90;
/// A status file older than this means the tray process isn't running.
pub const STATUS_ALIVE_SECS: i64 = 12;

/// `<state>/app-status.json`.
pub fn status_file(paths: &Paths) -> PathBuf {
    paths.state_dir.join("app-status.json")
}

/// `<state>/app-commands`.
pub fn commands_file(paths: &Paths) -> PathBuf {
    paths.state_dir.join("app-commands")
}

/// `<cache>/device-firmware.json`: what the last flash or `info` probe saw.
pub fn firmware_record_file(paths: &Paths) -> PathBuf {
    paths.cache_dir.join("device-firmware.json")
}

/// One account's last poll error. A status file from an older build (errors
/// as `[dir, message]` pairs) still reads, as not retrying.
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct AccountError {
    /// Config dir.
    pub dir: String,
    /// What to tell the user.
    pub message: String,
    /// The app retries on its own (shown as "Retrying"); otherwise the user
    /// has to act.
    #[serde(default)]
    pub retrying: bool,
}

/// The window-facing view of [`Status`].
#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
pub struct StatusSnapshot {
    /// Unix seconds this snapshot was written.
    pub updated_at: i64,
    /// `connected` | `connecting` | `searching` | `no_adapter` | `starting`.
    pub link: String,
    /// Device address / id when connected.
    pub device_key: Option<String>,
    /// Device name when connected.
    pub device_name: Option<String>,
    /// Adapter problem text (`no_adapter`).
    pub link_detail: Option<String>,
    /// Account the device shows.
    pub account: Option<String>,
    /// Session %.
    pub session_pct: Option<u32>,
    /// Weekly %.
    pub week_pct: Option<u32>,
    /// The session window has started.
    pub window_started: bool,
    /// Unix seconds the numbers were read (age = now - this).
    pub read_at: Option<i64>,
    /// Shown account missed three polls.
    pub stale: bool,
    /// Every account is logged out.
    pub logged_out: bool,
    /// Per-account errors.
    pub errors: Vec<AccountError>,
    /// Attention alert up.
    pub attention: bool,
    /// Hook activity 0-3.
    pub activity: u8,
    /// Sessions active in the last 5 min.
    pub sessions: u32,
    /// Hook events have been seen.
    pub hooks: bool,
    /// `claude` CLI in use.
    pub claude: Option<String>,
    /// Last successful device write.
    pub last_write_at: Option<i64>,
    /// Alert chimes muted for this run (tray menu).
    #[serde(alias = "beep_muted")]
    pub chimes_muted: bool,
    /// Latest firmware release, when the daily check found one.
    pub latest_firmware: Option<String>,
    /// Firmware the device last reported (flash or `info`).
    pub device_firmware: Option<String>,
    /// Tray state name (see [`TrayState`]).
    pub tray: String,
    /// The earliest a refresh can poll the shown account (the per-account
    /// gap and backoff); `None`: right away.
    #[serde(default)]
    pub refresh_at: Option<i64>,
    /// The usage endpoint blocked the shown account until then (a 429).
    #[serde(default)]
    pub paused_until: Option<i64>,
    /// The shown account is being polled.
    #[serde(default)]
    pub polling: bool,
    /// No tray icon: the window is the app, and closing it quits.
    #[serde(default)]
    pub windowed: bool,
    /// Unix seconds the session window resets (`None`: not started).
    #[serde(default)]
    pub session_resets_at: Option<i64>,
    /// Unix seconds the weekly window resets (`None`: unknown).
    #[serde(default)]
    pub week_resets_at: Option<i64>,
}

impl StatusSnapshot {
    /// Build from the daemon's status.
    pub fn from_status(s: &Status, now: i64) -> Self {
        let (link, device_key, device_name, link_detail) = match &s.link {
            LinkState::Starting => ("starting", None, None, None),
            LinkState::NoAdapter(e) => ("no_adapter", None, None, Some(e.clone())),
            LinkState::Searching => ("searching", None, None, None),
            LinkState::Connecting => ("connecting", None, None, None),
            LinkState::Connected { key, name } => ("connected", Some(key.clone()), name.clone(), None),
        };
        let e = &s.engine;
        let reading = e.reading.as_ref();
        StatusSnapshot {
            updated_at: now,
            link: link.into(),
            device_key,
            device_name,
            link_detail,
            account: e.active_account.as_ref().map(|p| p.display().to_string()),
            session_pct: reading.map(|r| r.session_pct()),
            week_pct: reading.and_then(|r| r.week.as_ref()).map(|w| w.pct),
            window_started: reading.is_some_and(|r| r.window_started()),
            read_at: e.age_secs.map(|a| now - a),
            stale: e.stale,
            logged_out: e.logged_out,
            errors: e
                .errors
                .iter()
                .map(|x| AccountError {
                    dir: x.dir.display().to_string(),
                    message: x.message.clone(),
                    retrying: x.retrying,
                })
                .collect(),
            attention: e.attention,
            activity: e.activity,
            sessions: e.sessions,
            hooks: e.hooks,
            claude: s.claude.as_ref().map(|p| p.display().to_string()),
            last_write_at: s.last_write_at,
            chimes_muted: false,
            latest_firmware: None,
            device_firmware: None,
            tray: format!("{:?}", tray_state(s, now)),
            refresh_at: e.refresh_at,
            paused_until: e.paused_until,
            polling: e.polling,
            windowed: false,
            session_resets_at: reading
                .and_then(|r| r.session.as_ref())
                .and_then(|w| w.resets_at)
                .map(|t| t.timestamp()),
            week_resets_at: reading.and_then(|r| r.week.as_ref()).and_then(|w| w.resets_at).map(|t| t.timestamp()),
        }
    }

    /// Seconds since the numbers were read.
    pub fn age(&self, now: i64) -> Option<i64> {
        self.read_at.map(|t| (now - t).max(0))
    }

    /// Equal apart from `updated_at`.
    pub fn same_content(&self, other: &StatusSnapshot) -> bool {
        StatusSnapshot { updated_at: 0, ..self.clone() } == StatusSnapshot { updated_at: 0, ..other.clone() }
    }

    /// The tray process is alive (the file was refreshed recently).
    pub fn alive(&self, now: i64) -> bool {
        now - self.updated_at <= STATUS_ALIVE_SECS
    }

    /// Parsed tray state.
    pub fn tray_state(&self) -> TrayState {
        match self.tray.as_str() {
            "Connected" => TrayState::Connected,
            "Stale" => TrayState::Stale,
            "AuthError" => TrayState::AuthError,
            "Attention" => TrayState::Attention,
            _ => TrayState::Searching,
        }
    }

    /// An update is available for the device.
    pub fn update_available(&self) -> Option<&str> {
        let latest = self.latest_firmware.as_deref()?;
        match self.device_firmware.as_deref() {
            // Local builds report `dev` (CLAWD_VERSION's default): not a
            // release, so nothing to compare against.
            Some(have) if !have.trim_start_matches(['v', 'V']).starts_with(|c: char| c.is_ascii_digit()) => None,
            Some(have) if !clawdmeter_flash::manifest::is_newer(latest, have) => None,
            Some(_) => Some(latest),
            None => None,
        }
    }
}

/// Attention wins, then auth, then the link, then freshness.
pub fn tray_state(s: &Status, now: i64) -> TrayState {
    if s.engine.attention {
        return TrayState::Attention;
    }
    if s.engine.logged_out {
        return TrayState::AuthError;
    }
    if !matches!(s.link, LinkState::Connected { .. }) {
        return TrayState::Searching;
    }
    let fresh = s.last_write_at.is_some_and(|t| now - t <= DATA_FRESH_SECS);
    if s.engine.stale || s.engine.reading.is_none() || !fresh {
        TrayState::Stale
    } else {
        TrayState::Connected
    }
}

/// Write atomically (tmp + rename).
pub fn write_status(paths: &Paths, snap: &StatusSnapshot) -> io::Result<()> {
    fs::create_dir_all(&paths.state_dir)?;
    let path = status_file(paths);
    let tmp = path.with_extension("json.tmp");
    fs::write(&tmp, serde_json::to_vec(snap).map_err(io::Error::other)?)?;
    fs::rename(tmp, path)
}

/// Read the latest snapshot (None when missing or unreadable).
pub fn read_status(paths: &Paths) -> Option<StatusSnapshot> {
    serde_json::from_slice(&fs::read(status_file(paths)).ok()?).ok()
}

/// Requests a window sends to the tray process.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum AppCommand {
    /// Poll `/usage` now.
    Refresh,
    /// Drop the BLE link and look again.
    Reconnect,
    /// Forget the cached device address, then look again.
    Forget,
    /// Mute / unmute the alert chimes for this run.
    Mute(bool),
    /// Open the app window on a page (see [`crate::Page::parse`]).
    Open(String),
    /// Re-run the firmware update check now.
    CheckUpdate,
    /// Stop the daemon and exit.
    Quit,
}

impl AppCommand {
    /// Wire form, one line.
    pub fn encode(&self) -> String {
        match self {
            AppCommand::Refresh => "refresh".into(),
            AppCommand::Reconnect => "reconnect".into(),
            AppCommand::Forget => "forget".into(),
            AppCommand::Mute(on) => format!("mute {}", if *on { "on" } else { "off" }),
            AppCommand::Open(w) => format!("open {w}"),
            AppCommand::CheckUpdate => "check-update".into(),
            AppCommand::Quit => "quit".into(),
        }
    }

    /// Parse one line.
    pub fn decode(line: &str) -> Option<Self> {
        let mut it = line.split_whitespace();
        Some(match (it.next()?, it.next()) {
            ("refresh", None) => AppCommand::Refresh,
            ("reconnect", None) => AppCommand::Reconnect,
            ("forget", None) => AppCommand::Forget,
            ("mute", Some("on")) => AppCommand::Mute(true),
            ("mute", Some("off")) => AppCommand::Mute(false),
            ("open", Some(w)) if crate::Page::parse(w).is_some() => AppCommand::Open(w.to_owned()),
            ("check-update", None) => AppCommand::CheckUpdate,
            ("quit", None) => AppCommand::Quit,
            _ => return None,
        })
    }
}

/// Append a command for the tray process.
pub fn send_command(paths: &Paths, cmd: &AppCommand) -> io::Result<()> {
    fs::create_dir_all(&paths.state_dir)?;
    let mut f = fs::OpenOptions::new().create(true).append(true).open(commands_file(paths))?;
    writeln!(f, "{}", cmd.encode())
}

/// Take every pending command (the file is renamed first, so a window
/// appending at the same moment lands in a fresh file, not a lost one).
pub fn take_commands(paths: &Paths) -> Vec<AppCommand> {
    let path = commands_file(paths);
    let taking = path.with_extension("taking");
    if fs::rename(&path, &taking).is_err() {
        return Vec::new();
    }
    let text = fs::read_to_string(&taking).unwrap_or_default();
    let _ = fs::remove_file(&taking);
    text.lines().filter_map(AppCommand::decode).collect()
}

/// `<state>/app.lock`: held for the life of the process that runs the daemon.
pub fn lock_file(paths: &Paths) -> PathBuf {
    paths.state_dir.join("app.lock")
}

/// Take the single-instance lock. `Ok(None)` means another tray or
/// `--daemon` process already runs the daemon (two would fight over the BLE
/// link and the command file). Keep the returned file open; the OS releases
/// the lock when the process exits, however it exits.
pub fn acquire_instance_lock(paths: &Paths) -> io::Result<Option<fs::File>> {
    fs::create_dir_all(&paths.state_dir)?;
    let f = fs::OpenOptions::new().create(true).truncate(false).write(true).open(lock_file(paths))?;
    match f.try_lock() {
        Ok(()) => Ok(Some(f)),
        Err(fs::TryLockError::WouldBlock) => Ok(None),
        Err(fs::TryLockError::Error(e)) => Err(e),
    }
}

/// `<state>/window-commands`.
pub fn window_commands_file(paths: &Paths) -> PathBuf {
    paths.state_dir.join("window-commands")
}

/// `<state>/window.lock`: held by the app window process.
pub fn window_lock_file(paths: &Paths) -> PathBuf {
    paths.state_dir.join("window.lock")
}

/// Ask the running window process to come to the front on `page`.
pub fn send_window_command(paths: &Paths, page: crate::Page) -> io::Result<()> {
    fs::create_dir_all(&paths.state_dir)?;
    let mut f = fs::OpenOptions::new().create(true).append(true).open(window_commands_file(paths))?;
    writeln!(f, "show {}", page.as_str())
}

/// Take every pending `show <page>` (rename first, as [`take_commands`]).
pub fn take_window_commands(paths: &Paths) -> Vec<crate::Page> {
    let path = window_commands_file(paths);
    let taking = path.with_extension("taking");
    if fs::rename(&path, &taking).is_err() {
        return Vec::new();
    }
    let text = fs::read_to_string(&taking).unwrap_or_default();
    let _ = fs::remove_file(&taking);
    text.lines().filter_map(|l| l.strip_prefix("show ")).filter_map(|p| crate::Page::parse(p.trim())).collect()
}

/// Take the window lock. `Ok(None)`: a window process is already running
/// (send it a [`send_window_command`] instead). Keep the file open.
pub fn acquire_window_lock(paths: &Paths) -> io::Result<Option<fs::File>> {
    fs::create_dir_all(&paths.state_dir)?;
    let f = fs::OpenOptions::new().create(true).truncate(false).write(true).open(window_lock_file(paths))?;
    match f.try_lock() {
        Ok(()) => Ok(Some(f)),
        Err(fs::TryLockError::WouldBlock) => Ok(None),
        Err(fs::TryLockError::Error(e)) => Err(e),
    }
}

/// Whether some process has the app window open (holds the window lock).
pub fn window_open_elsewhere(paths: &Paths) -> bool {
    matches!(acquire_window_lock(paths), Ok(None))
}

/// What the device last told us about its firmware.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct FirmwareRecord {
    /// PlatformIO env.
    pub env: String,
    /// Version.
    pub ver: String,
}

/// Remember the device's env and version (after a flash or an `info` probe).
pub fn save_firmware_record(paths: &Paths, rec: &FirmwareRecord) -> io::Result<()> {
    fs::create_dir_all(&paths.cache_dir)?;
    fs::write(firmware_record_file(paths), serde_json::to_vec(rec).map_err(io::Error::other)?)
}

/// The remembered firmware, if any.
pub fn load_firmware_record(paths: &Paths) -> Option<FirmwareRecord> {
    serde_json::from_slice(&fs::read(firmware_record_file(paths)).ok()?).ok()
}

/// `<cache>/latest-release`: tag of the last release the update check saw.
pub fn latest_release_file(paths: &Paths) -> PathBuf {
    paths.cache_dir.join("latest-release")
}

/// Read the cached latest release tag.
pub fn load_latest_release(paths: &Paths) -> Option<String> {
    fs::read_to_string(latest_release_file(paths)).ok().map(|s| s.trim().to_owned()).filter(|s| !s.is_empty())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn window_open_elsewhere_follows_the_lock() {
        let dir = std::env::temp_dir().join(format!("clawdmeter-winlock-{}", std::process::id()));
        let paths = Paths::under(&dir);
        assert!(!window_open_elsewhere(&paths));
        let held = acquire_window_lock(&paths).unwrap().expect("free");
        assert!(window_open_elsewhere(&paths));
        drop(held);
        assert!(!window_open_elsewhere(&paths));
        let _ = fs::remove_dir_all(dir);
    }
    use clawdmeter_core::usage::{UsageReading, Window};
    use clawdmeter_core::EngineStatus;

    fn status(link: LinkState, reading: bool, last_write: Option<i64>) -> Status {
        Status {
            link,
            engine: EngineStatus {
                reading: reading.then(|| UsageReading {
                    session: Some(Window { pct: 3, resets_at: None }),
                    week: Some(Window { pct: 12, resets_at: None }),
                    ..Default::default()
                }),
                ..Default::default()
            },
            last_write_at: last_write,
            claude: None,
        }
    }

    fn connected() -> LinkState {
        LinkState::Connected { key: "AA:BB".into(), name: Some("Clawdmeter".into()) }
    }

    #[test]
    fn tray_state_priorities() {
        let now = 1_000;
        assert_eq!(tray_state(&status(connected(), true, Some(now - 10)), now), TrayState::Connected);
        assert_eq!(tray_state(&status(connected(), true, Some(now - 91)), now), TrayState::Stale);
        assert_eq!(tray_state(&status(connected(), false, Some(now)), now), TrayState::Stale);
        assert_eq!(tray_state(&status(LinkState::Searching, true, Some(now)), now), TrayState::Searching);
        let mut s = status(connected(), true, Some(now));
        s.engine.logged_out = true;
        assert_eq!(tray_state(&s, now), TrayState::AuthError);
        s.engine.attention = true;
        assert_eq!(tray_state(&s, now), TrayState::Attention);
    }

    #[test]
    fn account_error_reads_old_pairs() {
        let old: Vec<AccountError> = serde_json::from_str(r#"[["/h/.claude","Not logged in."]]"#).unwrap();
        assert_eq!(
            old[0],
            AccountError { dir: "/h/.claude".into(), message: "Not logged in.".into(), retrying: false }
        );
        let e = AccountError { dir: "/d".into(), message: "m".into(), retrying: true };
        let back: AccountError = serde_json::from_str(&serde_json::to_string(&e).unwrap()).unwrap();
        assert_eq!(back, e);
    }

    #[test]
    fn snapshot_roundtrip() {
        let dir = std::env::temp_dir().join(format!("clawdmeter-ipc-{}", std::process::id()));
        let paths = Paths::under(&dir);
        let snap = StatusSnapshot::from_status(&status(connected(), true, Some(990)), 1_000);
        assert_eq!(snap.tray_state(), TrayState::Connected);
        assert_eq!((snap.session_pct, snap.week_pct), (Some(3), Some(12)));
        write_status(&paths, &snap).unwrap();
        let back = read_status(&paths).unwrap();
        assert_eq!(back, snap);
        assert!(back.alive(1_003));
        assert!(!back.alive(1_000 + STATUS_ALIVE_SECS + 1));
        let _ = fs::remove_dir_all(dir);
    }

    #[test]
    fn commands_roundtrip_and_take() {
        let dir = std::env::temp_dir().join(format!("clawdmeter-cmd-{}", std::process::id()));
        let paths = Paths::under(&dir);
        let cmds = [
            AppCommand::Refresh,
            AppCommand::Mute(true),
            AppCommand::Open("hooks".into()),
            AppCommand::Forget,
            AppCommand::Quit,
        ];
        for c in &cmds {
            assert_eq!(AppCommand::decode(&c.encode()).as_ref(), Some(c));
            send_command(&paths, c).unwrap();
        }
        assert_eq!(AppCommand::decode("open nowhere"), None);
        assert_eq!(AppCommand::decode("rm -rf"), None);
        assert_eq!(take_commands(&paths), cmds.to_vec());
        assert!(take_commands(&paths).is_empty());
        let _ = fs::remove_dir_all(dir);
    }

    #[test]
    fn window_commands_and_lock() {
        let dir = std::env::temp_dir().join(format!("clawdmeter-win-{}", std::process::id()));
        let paths = Paths::under(&dir);
        send_window_command(&paths, crate::Page::Hooks).unwrap();
        send_window_command(&paths, crate::Page::Flash).unwrap();
        assert_eq!(take_window_commands(&paths), [crate::Page::Hooks, crate::Page::Flash]);
        assert!(take_window_commands(&paths).is_empty());
        let first = acquire_window_lock(&paths).unwrap();
        assert!(first.is_some());
        assert!(acquire_window_lock(&paths).unwrap().is_none());
        drop(first);
        let _ = fs::remove_dir_all(dir);
    }

    #[test]
    fn instance_lock_is_exclusive() {
        let dir = std::env::temp_dir().join(format!("clawdmeter-lock-{}", std::process::id()));
        let paths = Paths::under(&dir);
        let first = acquire_instance_lock(&paths).unwrap();
        assert!(first.is_some());
        assert!(acquire_instance_lock(&paths).unwrap().is_none());
        drop(first);
        assert!(acquire_instance_lock(&paths).unwrap().is_some());
        let _ = fs::remove_dir_all(dir);
    }

    #[test]
    fn update_available_needs_a_known_device_version() {
        let mut s = StatusSnapshot { latest_firmware: Some("v1.9.0".into()), ..Default::default() };
        assert_eq!(s.update_available(), None);
        s.device_firmware = Some("1.8.2".into());
        assert_eq!(s.update_available(), Some("v1.9.0"));
        s.device_firmware = Some("1.9.0".into());
        assert_eq!(s.update_available(), None);
        s.device_firmware = Some("dev".into());
        assert_eq!(s.update_available(), None, "a local build is not offered a release");
    }
}
