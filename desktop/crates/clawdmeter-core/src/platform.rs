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

    fn from_u8(v: u8) -> Self {
        if v == 1 {
            Appearance::Light
        } else {
            Appearance::Dark
        }
    }
}

/// The OS's appearance right now, `None` when it can't be read. Linux asks
/// the desktop portal over D-Bus, macOS reads `AppleInterfaceStyle`, Windows
/// the `AppsUseLightTheme` registry value. This blocks for the read; to
/// follow changes, use [`AppearanceWatch`].
pub fn system_appearance() -> Option<Appearance> {
    Reader::default().read()
}

/// How often [`AppearanceWatch`] asks the OS: a D-Bus or registry read is
/// cheap, macOS spawns `defaults`.
const APPEARANCE_EVERY: Duration =
    if cfg!(target_os = "macos") { Duration::from_secs(2) } else { Duration::from_millis(500) };
/// After a failed read (no portal yet, say), how long before asking again.
const APPEARANCE_RETRY: Duration = Duration::from_secs(5);

/// The OS's appearance, kept current by a background thread so reading it
/// never blocks (the daemon's runtime and the window's UI thread both ask).
/// It keeps the last answer through a failed read; dark before any. The
/// thread ends after the watch is dropped.
pub struct AppearanceWatch {
    state: Arc<AtomicU8>,
}

impl AppearanceWatch {
    /// Start watching. The first answer arrives within one read.
    pub fn spawn() -> Self {
        let state = Arc::new(AtomicU8::new(Appearance::default().to_u8()));
        let shared = Arc::downgrade(&state);
        let started = std::thread::Builder::new().name("appearance".into()).spawn(move || {
            let mut reader = Reader::default();
            watch_loop(&shared, || reader.read(), std::thread::sleep);
        });
        if let Err(e) = started {
            log::warn!("appearance: no watch thread ({e}); the System theme stays dark");
        }
        AppearanceWatch { state }
    }

    /// The appearance as last read.
    pub fn get(&self) -> Appearance {
        Appearance::from_u8(self.state.load(Ordering::Relaxed))
    }
}

/// Read, store and wait until the watch is gone: [`APPEARANCE_EVERY`]
/// after an answer, [`APPEARANCE_RETRY`] after a miss (the last answer
/// stays).
fn watch_loop(state: &Weak<AtomicU8>, mut read: impl FnMut() -> Option<Appearance>, mut sleep: impl FnMut(Duration)) {
    loop {
        let got = read();
        let Some(state) = state.upgrade() else { return };
        if let Some(a) = got {
            state.store(a.to_u8(), Ordering::Relaxed);
        }
        drop(state);
        sleep(if got.is_some() { APPEARANCE_EVERY } else { APPEARANCE_RETRY });
    }
}

/// One way of asking the OS, keeping what it can between reads (the D-Bus
/// connection).
#[derive(Default)]
struct Reader {
    #[cfg(target_os = "linux")]
    conn: Option<dbus::blocking::Connection>,
}

impl Reader {
    #[cfg(target_os = "linux")]
    fn read(&mut self) -> Option<Appearance> {
        use dbus::arg::{RefArg, Variant};
        // Unwraps the portal's variant (two deep from the older `Read`).
        fn number(a: &dyn RefArg) -> Option<u64> {
            a.as_u64().or_else(|| a.as_iter()?.next().and_then(number))
        }
        if self.conn.is_none() {
            self.conn = dbus::blocking::Connection::new_session().ok();
        }
        let conn = self.conn.as_ref()?;
        let portal = conn.with_proxy(
            "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",
            Duration::from_millis(250),
        );
        let args = ("org.freedesktop.appearance", "color-scheme");
        let read = |method| -> Result<(Variant<Box<dyn RefArg>>,), dbus::Error> {
            portal.method_call("org.freedesktop.portal.Settings", method, args)
        };
        match read("ReadOne").or_else(|_| read("Read")) {
            Ok((v,)) => Some(number(&v).and_then(Appearance::from_portal).unwrap_or_default()),
            Err(e) => {
                log::debug!("appearance: portal unreadable ({e})");
                // A fresh connection next time, in case the bus went away.
                self.conn = None;
                None
            }
        }
    }

    #[cfg(target_os = "macos")]
    fn read(&mut self) -> Option<Appearance> {
        // The key exists ("Dark") only in dark mode; `defaults` fails when
        // it's absent, which is light mode.
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
    fn read(&mut self) -> Option<Appearance> {
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
    fn read(&mut self) -> Option<Appearance> {
        None
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
    fn the_watch_keeps_the_last_answer_and_stops_when_dropped() {
        use std::cell::RefCell;
        let state = Arc::new(AtomicU8::new(Appearance::default().to_u8()));
        let weak = Arc::downgrade(&state);
        let answers = RefCell::new(vec![None, Some(Appearance::Light), None, Some(Appearance::Dark)].into_iter());
        let seen = RefCell::new(Vec::new());
        let slept = RefCell::new(Vec::new());
        let holder = RefCell::new(Some(state));
        let get = || Appearance::from_u8(holder.borrow().as_ref().unwrap().load(Ordering::Relaxed));
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
        assert_eq!(*seen.borrow(), [Dark, Light, Light, Dark], "dark before any answer; a miss keeps the last");
        let (every, retry) = (APPEARANCE_EVERY, APPEARANCE_RETRY);
        assert_eq!(*slept.borrow(), [retry, every, retry, every]);
    }
}
