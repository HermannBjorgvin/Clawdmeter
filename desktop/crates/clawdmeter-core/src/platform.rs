//! Small host queries: hour format, UTC offset, light/dark appearance and,
//! on Linux, desktop portal settings.

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

/// A string setting from the desktop portal
/// (`org.freedesktop.portal.Settings`), such as GNOME's
/// `org.gnome.desktop.wm.preferences button-layout`, kept current by a
/// background thread so reading it never blocks: one read, then the
/// portal's `SettingChanged` signal. A missed read is retried as the
/// appearance watch retries it, so a portal still starting at login
/// answers late rather than never; a portal that answers it has no such
/// setting isn't asked again, since a value added later arrives as
/// `SettingChanged`. Each answer goes through `parse` on that
/// thread: the string, or `None` when the portal has no such setting (or it
/// isn't a string). The thread ends after the watch is dropped.
#[cfg(target_os = "linux")]
pub struct SettingWatch<T> {
    answer: Arc<Answer<T>>,
}

#[cfg(target_os = "linux")]
impl<T: Clone + Send + 'static> SettingWatch<T> {
    /// Start watching `namespace` `key`.
    pub fn spawn(namespace: &'static str, key: &'static str, parse: fn(Option<&str>) -> T) -> Self {
        let answer = Arc::new(Answer::new());
        let shared = Arc::downgrade(&answer);
        let started = std::thread::Builder::new().name(format!("portal {key}")).spawn(move || {
            let alive = shared.clone();
            portal::follow(
                namespace,
                key,
                move || alive.strong_count() > 0,
                move |v| {
                    if let Some(a) = shared.upgrade() {
                        a.set(parse(v.and_then(portal::string).as_deref()));
                    }
                },
            );
        });
        if let Err(e) = started {
            log::warn!("portal: no thread to watch {namespace} {key} ({e})");
        }
        SettingWatch { answer }
    }

    /// The setting as last parsed, `None` until the portal has answered.
    pub fn get(&self) -> Option<T> {
        self.answer.get()
    }

    /// [`get`](Self::get), first waiting up to `timeout` for the portal's
    /// first answer.
    pub fn wait(&self, timeout: Duration) -> Option<T> {
        self.answer.wait(timeout)
    }
}

/// A watch's latest answer, and a wake-up for whoever waits on the first.
#[cfg(target_os = "linux")]
struct Answer<T> {
    value: std::sync::Mutex<Option<T>>,
    first: std::sync::Condvar,
}

#[cfg(target_os = "linux")]
impl<T: Clone> Answer<T> {
    fn new() -> Self {
        Answer { value: std::sync::Mutex::new(None), first: std::sync::Condvar::new() }
    }

    /// The value is a plain answer: a panic mid-update can't leave it torn.
    fn lock(&self) -> std::sync::MutexGuard<'_, Option<T>> {
        self.value.lock().unwrap_or_else(std::sync::PoisonError::into_inner)
    }

    fn set(&self, v: T) {
        *self.lock() = Some(v);
        self.first.notify_all();
    }

    fn get(&self) -> Option<T> {
        self.lock().clone()
    }

    fn wait(&self, timeout: Duration) -> Option<T> {
        let (value, _) = self
            .first
            .wait_timeout_while(self.lock(), timeout, |v| v.is_none())
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        value.clone()
    }
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
    /// No portal, no settings interface, or one that refuses us: ask again
    /// rarely, on the same connection (a portal can start later).
    Absent,
    /// The portal answered that it has no such setting, as KDE's and
    /// wlroots' portals do for GNOME's keys. A value added later arrives as
    /// `SettingChanged`, so it isn't asked again.
    Unset,
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
            | "org.freedesktop.DBus.Error.UnknownInterface" => Miss::Absent,
            "org.freedesktop.portal.Error.NotFound" => Miss::Unset,
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
/// color-scheme`, and any setting a [`SettingWatch`] follows.
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
    /// Before connecting again when there's no bus, and the most a lost
    /// connection waits (see [`reconnect_delay`]).
    const RECONNECT: Duration = Duration::from_secs(30);

    /// Before reconnecting after `lost` connections in a row went away:
    /// 1 s, doubling up to [`RECONNECT`]. Without it, a bus that accepts
    /// connections but fails every call would be dialled in a tight loop.
    pub(super) fn reconnect_delay(lost: u32) -> Duration {
        Duration::from_secs(1 << lost.saturating_sub(1).min(5)).min(RECONNECT)
    }

    /// Unwraps the portal's variant (two deep from the older `Read`).
    fn number(a: &dyn RefArg) -> Option<u64> {
        a.as_u64().or_else(|| a.as_iter()?.next().and_then(number))
    }

    /// Unwraps a string the same way.
    pub(super) fn string(a: &dyn RefArg) -> Option<String> {
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

    /// Follow the appearance until the watch is gone.
    pub(super) fn watch(state: &Weak<AtomicU8>) {
        let alive = state.clone();
        let w = state.clone();
        follow(
            NAMESPACE,
            KEY,
            move || alive.strong_count() > 0,
            move |v| {
                if let Some(v) = v {
                    store(&w, appearance(v));
                }
            },
        );
    }

    /// Follow `namespace` `key` while `alive`: subscribe to
    /// `SettingChanged`, read it once, then wait for signals. `on` gets
    /// each value, or `None` when there's no such setting. A portal that
    /// answers it has none ([`Miss::Unset`]) isn't asked again; with no
    /// portal at all ([`Miss::Absent`]) it's asked every minute; a transient
    /// miss is retried without telling `on`. Only a lost bus reconnects,
    /// after [`reconnect_delay`].
    pub(super) fn follow<F>(namespace: &'static str, key: &'static str, alive: impl Fn() -> bool, on: F)
    where
        F: Fn(Option<&dyn RefArg>) + Clone + Send + 'static,
    {
        // Connections lost in a row since the portal last answered.
        let mut lost = 0;
        while alive() {
            let Ok(conn) = Connection::new_session() else {
                log::debug!("portal {key}: no session bus");
                std::thread::sleep(RECONNECT);
                continue;
            };
            let signal = on.clone();
            let rule = MatchRule::new_signal(IFACE, "SettingChanged").with_path(PATH);
            let subscribed =
                conn.add_match(rule, move |(ns, k, v): (String, String, Variant<Box<dyn RefArg>>), _, _| {
                    if ns == namespace && k == key {
                        signal(Some(&v));
                    }
                    true
                });
            if let Err(e) = subscribed {
                log::debug!("portal {key}: can't subscribe ({e})");
                std::thread::sleep(RECONNECT);
                continue;
            }
            let mut next_read = Some(Instant::now());
            let mut misses = 0;
            while alive() {
                if next_read.is_some_and(|t| Instant::now() >= t) {
                    match read_value(&conn, namespace, key) {
                        Ok(v) => {
                            on(Some(&v));
                            next_read = None;
                            lost = 0;
                        }
                        Err(Miss::Unset) => {
                            on(None);
                            next_read = None;
                            lost = 0;
                        }
                        Err(Miss::Disconnected) => break,
                        Err(m) => {
                            if m == Miss::Absent {
                                on(None);
                            }
                            misses += 1;
                            next_read = Some(Instant::now() + m.retry_in(misses));
                        }
                    }
                }
                if conn.process(WAIT).is_err() {
                    break; // the bus went away
                }
            }
            lost += 1;
            if alive() {
                std::thread::sleep(reconnect_delay(lost));
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

    #[cfg(target_os = "linux")]
    #[test]
    fn reconnects_back_off() {
        let secs: Vec<u64> = (1..=8).map(|n| portal::reconnect_delay(n).as_secs()).collect();
        assert_eq!(secs, [1, 2, 4, 8, 16, 30, 30, 30], "1 s, doubling, capped at RECONNECT");
        assert_eq!(portal::reconnect_delay(u32::MAX).as_secs(), 30, "no overflow however many are lost");
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn setting_answers() {
        let a = Answer::<u8>::new();
        assert_eq!(a.get(), None);
        assert_eq!(a.wait(Duration::from_millis(20)), None, "no answer: the wait runs out");
        std::thread::scope(|s| {
            s.spawn(|| {
                std::thread::sleep(Duration::from_millis(20));
                a.set(7);
            });
            assert_eq!(a.wait(Duration::from_secs(10)), Some(7), "the first answer ends the wait");
        });
        a.set(9);
        assert_eq!(a.get(), Some(9), "a later answer replaces it");
        assert_eq!(a.wait(Duration::ZERO), Some(9), "an answered watch doesn't wait");
    }

    #[test]
    fn portal_misses() {
        assert_eq!(Miss::from_error(Some("org.freedesktop.DBus.Error.ServiceUnknown")), Miss::Absent);
        assert_eq!(
            Miss::from_error(Some("org.freedesktop.portal.Error.NotFound")),
            Miss::Unset,
            "the portal answered: no such setting, so it isn't asked again"
        );
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
