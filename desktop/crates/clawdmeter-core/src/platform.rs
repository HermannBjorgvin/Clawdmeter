//! Small host queries: hour format, UTC offset and light/dark appearance.

use std::process::{Command, Stdio};
use std::sync::atomic::{AtomicU8, Ordering};
use std::sync::{Arc, Weak};
use std::time::Duration;

fn run(cmd: &str, args: &[&str]) -> Option<String> {
    let out = crate::autostart::no_console(Command::new(cmd).args(args))
        .stdin(Stdio::null())
        .stderr(Stdio::null())
        .output()
        .ok()?;
    out.status.success().then(|| String::from_utf8_lossy(&out.stdout).into_owned())
}

/// True when a `t_fmt` / `sShortTime`-style format is 12-hour.
pub fn format_is_12h(fmt: &str) -> bool {
    fmt.contains("%p") || fmt.contains("%r") || fmt.contains("%I") || fmt.contains("tt")
}

/// Best-effort 12h/24h for `clock = auto`
/// (macOS defaults toggles, then `locale -k LC_TIME`; Windows `iTime`).
/// Defaults to 24h.
pub fn prefers_12h() -> bool {
    if cfg!(target_os = "macos") {
        if run("defaults", &["read", "-g", "AppleICUForce24HourTime"]).is_some_and(|s| s.trim() == "1") {
            return false;
        }
        if run("defaults", &["read", "-g", "AppleICUForce12HourTime"]).is_some_and(|s| s.trim() == "1") {
            return true;
        }
    }
    if cfg!(windows) {
        // iTime: "1" = 24-hour, "0" = 12-hour.
        return run("reg", &["query", r"HKCU\Control Panel\International", "/v", "iTime"])
            .is_some_and(|s| s.lines().any(|l| l.contains("iTime") && l.trim_end().ends_with('0')));
    }
    run("locale", &["-k", "LC_TIME"])
        .and_then(|s| s.lines().find(|l| l.starts_with("t_fmt=")).map(format_is_12h))
        .unwrap_or(false)
}

/// The local UTC offset in seconds right now.
pub fn utc_offset_secs() -> i64 {
    i64::from(chrono::Local::now().offset().local_minus_utc())
}

/// The OS's light or dark mode. Unknown counts as dark.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum Appearance {
    /// Dark mode, or not known.
    #[default]
    Dark,
    /// Light mode.
    Light,
}

impl Appearance {
    /// The xdg-desktop-portal `org.freedesktop.appearance color-scheme`
    /// value: 1 prefers dark, 2 prefers light, 0 is no preference, which is
    /// how GNOME reports its (light) default style and what libadwaita and
    /// gpui draw light.
    pub fn from_portal(v: u64) -> Option<Self> {
        match v {
            1 => Some(Appearance::Dark),
            0 | 2 => Some(Appearance::Light),
            _ => None,
        }
    }

    fn to_u8(self) -> u8 {
        match self {
            Appearance::Dark => 0,
            Appearance::Light => 1,
        }
    }

    /// `None` for [`UNKNOWN`].
    fn from_u8(v: u8) -> Option<Self> {
        match v {
            0 => Some(Appearance::Dark),
            1 => Some(Appearance::Light),
            _ => None,
        }
    }
}

/// The watch's state before its first answer.
const UNKNOWN: u8 = u8::MAX;

/// The OS's appearance right now, `None` when it can't be read. Linux asks
/// the desktop portal over D-Bus, macOS reads `AppleInterfaceStyle`, Windows
/// the `AppsUseLightTheme` registry value. This blocks for the read; to
/// follow changes, use [`AppearanceWatch`].
pub fn system_appearance() -> Option<Appearance> {
    #[cfg(target_os = "linux")]
    {
        let conn = dbus::blocking::Connection::new_session().ok()?;
        portal::read(&conn).ok()
    }
    #[cfg(not(target_os = "linux"))]
    {
        poll_read()
    }
}

/// A string from the desktop portal's settings
/// (`org.freedesktop.portal.Settings`), such as GNOME's
/// `org.gnome.desktop.wm.preferences button-layout`. `None` when the portal,
/// the namespace or the key is missing, or the value isn't a string. Blocks
/// for the read, at most two seconds.
#[cfg(target_os = "linux")]
pub fn portal_string(namespace: &str, key: &str) -> Option<String> {
    portal::read_string(namespace, key)
}

/// How often the watch asks on macOS (each read spawns `defaults`) and
/// Windows (a registry read). Linux is told of changes instead.
#[cfg(any(not(target_os = "linux"), test))]
const APPEARANCE_EVERY: Duration =
    if cfg!(target_os = "macos") { Duration::from_secs(10) } else { Duration::from_secs(2) };
/// After a failed read, how long before asking again.
#[cfg(any(not(target_os = "linux"), test))]
const APPEARANCE_RETRY: Duration = Duration::from_secs(60);

/// The OS's appearance, kept current by a background thread so reading it
/// never blocks (the daemon's runtime and the window's UI thread both ask).
/// Linux reads the desktop portal once and then follows its
/// `SettingChanged` signal; macOS and Windows poll. It keeps the last answer
/// through a failed read; dark before any. The thread ends after the watch
/// is dropped.
pub struct AppearanceWatch {
    state: Arc<AtomicU8>,
}

impl AppearanceWatch {
    /// Start watching. The first answer arrives within one read.
    pub fn spawn() -> Self {
        let state = Arc::new(AtomicU8::new(UNKNOWN));
        let shared = Arc::downgrade(&state);
        let started = std::thread::Builder::new().name("appearance".into()).spawn(move || {
            #[cfg(target_os = "linux")]
            portal::watch(&shared);
            #[cfg(not(target_os = "linux"))]
            watch_loop(&shared, poll_read, std::thread::sleep);
        });
        if let Err(e) = started {
            log::warn!("appearance: no watch thread ({e}); the System theme stays dark");
        }
        AppearanceWatch { state }
    }

    /// The appearance as last read; Dark before the first answer.
    pub fn get(&self) -> Appearance {
        self.known().unwrap_or_default()
    }

    /// The appearance as last read, `None` until the OS has answered once.
    pub fn known(&self) -> Option<Appearance> {
        Appearance::from_u8(self.state.load(Ordering::Relaxed))
    }
}

/// Store `a` if the watch is still there.
fn store(state: &Weak<AtomicU8>, a: Appearance) {
    if let Some(s) = state.upgrade() {
        s.store(a.to_u8(), Ordering::Relaxed);
    }
}

/// Poll until the watch is gone: [`APPEARANCE_EVERY`] after an answer,
/// [`APPEARANCE_RETRY`] after a miss (the last answer stays).
#[cfg(any(not(target_os = "linux"), test))]
fn watch_loop(state: &Weak<AtomicU8>, mut read: impl FnMut() -> Option<Appearance>, mut sleep: impl FnMut(Duration)) {
    loop {
        let got = read();
        if state.strong_count() == 0 {
            return;
        }
        if let Some(a) = got {
            store(state, a);
        }
        sleep(if got.is_some() { APPEARANCE_EVERY } else { APPEARANCE_RETRY });
    }
}

#[cfg(target_os = "macos")]
fn poll_read() -> Option<Appearance> {
    // The key exists ("Dark") only in dark mode; `defaults` fails when it's
    // absent, which is light mode.
    let out = Command::new("defaults")
        .args(["read", "-g", "AppleInterfaceStyle"])
        .stdin(Stdio::null())
        .stderr(Stdio::null())
        .output()
        .ok()?;
    let dark = out.status.success() && String::from_utf8_lossy(&out.stdout).trim().eq_ignore_ascii_case("dark");
    Some(if dark { Appearance::Dark } else { Appearance::Light })
}

#[cfg(windows)]
fn poll_read() -> Option<Appearance> {
    use windows_sys::Win32::System::Registry::{RegGetValueW, HKEY_CURRENT_USER, RRF_RT_REG_DWORD};
    let wide = |s: &str| s.encode_utf16().chain(Some(0)).collect::<Vec<u16>>();
    let key = wide(r"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize");
    let value = wide("AppsUseLightTheme");
    let mut data: u32 = 0;
    let mut size = std::mem::size_of::<u32>() as u32;
    // SAFETY: both names are NUL-terminated and outlive the call; `data`
    // holds the REG_DWORD the flags ask for and `size` says how big it is.
    let status = unsafe {
        RegGetValueW(
            HKEY_CURRENT_USER,
            key.as_ptr(),
            value.as_ptr(),
            RRF_RT_REG_DWORD,
            std::ptr::null_mut(),
            (&mut data as *mut u32).cast(),
            &mut size,
        )
    };
    (status == 0).then_some(if data == 0 { Appearance::Dark } else { Appearance::Light })
}

#[cfg(not(any(target_os = "linux", target_os = "macos", windows)))]
fn poll_read() -> Option<Appearance> {
    None
}

/// What a failed portal read means for the next one.
#[cfg(any(target_os = "linux", test))]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Miss {
    /// The session bus went away: reconnect.
    Disconnected,
    /// No portal, no appearance backend, or the key unknown: ask again
    /// rarely, on the same connection.
    Absent,
    /// The portal predates `ReadOne`: use `Read`.
    OldPortal,
    /// Anything else (a timeout, say): ask again soon.
    Transient,
}

#[cfg(any(target_os = "linux", test))]
impl Miss {
    /// From a D-Bus error name.
    fn from_error(name: Option<&str>) -> Self {
        match name.unwrap_or("") {
            "org.freedesktop.DBus.Error.Disconnected" | "org.freedesktop.DBus.Error.NoServer" => Miss::Disconnected,
            // A portal that can't start, or a policy that refuses it, is as
            // good as none: don't make the bus spawn it every few seconds.
            n if n.starts_with("org.freedesktop.DBus.Error.Spawn.") => Miss::Absent,
            "org.freedesktop.DBus.Error.ServiceUnknown"
            | "org.freedesktop.DBus.Error.AccessDenied"
            | "org.freedesktop.DBus.Error.NameHasNoOwner"
            | "org.freedesktop.DBus.Error.UnknownObject"
            | "org.freedesktop.DBus.Error.UnknownInterface"
            | "org.freedesktop.portal.Error.NotFound" => Miss::Absent,
            "org.freedesktop.DBus.Error.UnknownMethod" => Miss::OldPortal,
            _ => Miss::Transient,
        }
    }

    /// How long before the next read, after `streak` misses in a row
    /// (1 for the first): a transient miss backs off 5 s, 30 s, then a
    /// minute.
    fn retry_in(self, streak: u32) -> Duration {
        match (self, streak) {
            (Miss::Absent, _) | (_, 3..) => Duration::from_secs(60),
            (_, 2) => Duration::from_secs(30),
            _ => Duration::from_secs(5),
        }
    }
}

/// The xdg-desktop-portal's settings: `org.freedesktop.appearance
/// color-scheme`, followed, and one-off string reads.
#[cfg(target_os = "linux")]
mod portal {
    use super::{store, Appearance, Miss};
    use dbus::arg::{RefArg, Variant};
    use dbus::blocking::Connection;
    use dbus::message::MatchRule;
    use std::sync::atomic::AtomicU8;
    use std::sync::Weak;
    use std::time::{Duration, Instant};

    const DEST: &str = "org.freedesktop.portal.Desktop";
    const PATH: &str = "/org/freedesktop/portal/desktop";
    const IFACE: &str = "org.freedesktop.portal.Settings";
    const NAMESPACE: &str = "org.freedesktop.appearance";
    const KEY: &str = "color-scheme";
    /// Long enough for the portal to start on demand at login, so the
    /// first answer isn't a timeout.
    const TIMEOUT: Duration = Duration::from_secs(2);
    /// How long one wait for signals lasts before checking the watch is
    /// still wanted.
    const WAIT: Duration = Duration::from_secs(2);
    /// Before connecting again after the bus went away (or wasn't there).
    const RECONNECT: Duration = Duration::from_secs(30);

    /// Unwraps the portal's variant (two deep from the older `Read`).
    fn number(a: &dyn RefArg) -> Option<u64> {
        a.as_u64().or_else(|| a.as_iter()?.next().and_then(number))
    }

    /// Unwraps a string the same way.
    fn string(a: &dyn RefArg) -> Option<String> {
        a.as_str().map(str::to_owned).or_else(|| a.as_iter()?.next().and_then(string))
    }

    fn appearance(v: &dyn RefArg) -> Appearance {
        number(v).and_then(Appearance::from_portal).unwrap_or_default()
    }

    /// Read one setting: `ReadOne`, or `Read` on a portal that predates it.
    fn read_value(conn: &Connection, namespace: &str, key: &str) -> Result<Variant<Box<dyn RefArg>>, Miss> {
        let portal = conn.with_proxy(DEST, PATH, TIMEOUT);
        let call = |method| -> Result<(Variant<Box<dyn RefArg>>,), Miss> {
            portal.method_call(IFACE, method, (namespace, key)).map_err(|e: dbus::Error| {
                log::debug!("portal {method} {namespace} {key}: {e}");
                Miss::from_error(e.name())
            })
        };
        let (v,) = match call("ReadOne") {
            Err(Miss::OldPortal) => call("Read")?,
            r => r?,
        };
        Ok(v)
    }

    /// Read the appearance once.
    pub(super) fn read(conn: &Connection) -> Result<Appearance, Miss> {
        read_value(conn, NAMESPACE, KEY).map(|v| appearance(&v))
    }

    /// Read a string setting once, on a connection of its own.
    pub(super) fn read_string(namespace: &str, key: &str) -> Option<String> {
        let conn = Connection::new_session().ok()?;
        string(&read_value(&conn, namespace, key).ok()?)
    }

    /// Follow the setting until the watch is gone: subscribe to
    /// `SettingChanged`, read it once, then wait for signals. A missed read
    /// is retried on the same connection; only a lost bus reconnects.
    pub(super) fn watch(state: &Weak<AtomicU8>) {
        while state.strong_count() > 0 {
            let Ok(conn) = Connection::new_session() else {
                log::debug!("appearance: no session bus");
                std::thread::sleep(RECONNECT);
                continue;
            };
            let w = state.clone();
            let rule = MatchRule::new_signal(IFACE, "SettingChanged").with_path(PATH);
            let subscribed =
                conn.add_match(rule, move |(ns, key, v): (String, String, Variant<Box<dyn RefArg>>), _, _| {
                    if ns == NAMESPACE && key == KEY {
                        store(&w, appearance(&v));
                    }
                    true
                });
            if let Err(e) = subscribed {
                log::debug!("appearance: can't subscribe ({e})");
                std::thread::sleep(RECONNECT);
                continue;
            }
            let mut next_read = Some(Instant::now());
            let mut misses = 0;
            while state.strong_count() > 0 {
                if next_read.is_some_and(|t| Instant::now() >= t) {
                    match read(&conn) {
                        Ok(a) => {
                            store(state, a);
                            next_read = None;
                        }
                        Err(Miss::Disconnected) => break,
                        Err(m) => {
                            misses += 1;
                            next_read = Some(Instant::now() + m.retry_in(misses));
                        }
                    }
                }
                if conn.process(WAIT).is_err() {
                    break; // the bus went away
                }
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn formats() {
        assert!(format_is_12h("t_fmt=\"%r\""));
        assert!(format_is_12h("%I:%M:%S %p"));
        assert!(!format_is_12h("t_fmt=\"%T\""));
        assert!(!format_is_12h("%H:%M:%S"));
    }

    #[test]
    fn portal_color_scheme() {
        assert_eq!(Appearance::from_portal(1), Some(Appearance::Dark));
        assert_eq!(Appearance::from_portal(2), Some(Appearance::Light));
        assert_eq!(Appearance::from_portal(0), Some(Appearance::Light), "no preference: GNOME's default, light");
        assert_eq!(Appearance::from_portal(7), None);
        assert_eq!(Appearance::default(), Appearance::Dark, "unknown is dark");
    }

    #[test]
    fn portal_misses() {
        assert_eq!(Miss::from_error(Some("org.freedesktop.DBus.Error.ServiceUnknown")), Miss::Absent);
        assert_eq!(Miss::from_error(Some("org.freedesktop.portal.Error.NotFound")), Miss::Absent);
        assert_eq!(Miss::from_error(Some("org.freedesktop.DBus.Error.UnknownMethod")), Miss::OldPortal);
        assert_eq!(Miss::from_error(Some("org.freedesktop.DBus.Error.Disconnected")), Miss::Disconnected);
        assert_eq!(Miss::from_error(Some("org.freedesktop.DBus.Error.NoReply")), Miss::Transient);
        assert_eq!(Miss::from_error(None), Miss::Transient);
        assert_eq!(Miss::from_error(Some("org.freedesktop.DBus.Error.Spawn.ChildExited")), Miss::Absent);
        assert_eq!(Miss::from_error(Some("org.freedesktop.DBus.Error.AccessDenied")), Miss::Absent);
        let secs = |m: Miss, n| m.retry_in(n).as_secs();
        assert_eq!(secs(Miss::Absent, 1), 60, "no portal: rarely");
        assert_eq!([1, 2, 3, 9].map(|n| secs(Miss::Transient, n)), [5, 30, 60, 60], "timeouts back off");
    }

    #[test]
    fn the_watch_keeps_the_last_answer_and_stops_when_dropped() {
        use std::cell::RefCell;
        let state = Arc::new(AtomicU8::new(UNKNOWN));
        let weak = Arc::downgrade(&state);
        let answers = RefCell::new(vec![None, Some(Appearance::Light), None, Some(Appearance::Dark)].into_iter());
        let seen = RefCell::new(Vec::new());
        let slept = RefCell::new(Vec::new());
        let holder = RefCell::new(Some(state));
        let get = || Appearance::from_u8(holder.borrow().as_ref().unwrap().load(Ordering::Relaxed));
        assert_eq!(get(), None, "unknown before any answer");
        watch_loop(
            &weak,
            || match answers.borrow_mut().next() {
                Some(a) => a,
                None => {
                    holder.borrow_mut().take(); // the watch is dropped
                    None
                }
            },
            |d| {
                slept.borrow_mut().push(d);
                seen.borrow_mut().push(get());
            },
        );
        use Appearance::{Dark, Light};
        assert_eq!(*seen.borrow(), [None, Some(Light), Some(Light), Some(Dark)], "a miss keeps the last answer");
        let (every, retry) = (APPEARANCE_EVERY, APPEARANCE_RETRY);
        assert_eq!(*slept.borrow(), [retry, every, retry, every]);
    }
}
