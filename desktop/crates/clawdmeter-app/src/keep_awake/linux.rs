//! Linux: systemd-logind's lid-switch inhibitor and the desktop's suspend
//! inhibitor.
//!
//! Two holds, both gone with the process, so there is nothing to restore
//! after a crash:
//!
//! - logind `Inhibit("handle-lid-switch", …, "block")` on the system bus.
//!   logind then leaves the lid switch alone; it hands back a file
//!   descriptor and drops the lock when that closes. polkit allows it for a
//!   local session without a prompt
//!   (`org.freedesktop.login1.inhibit-handle-lid-switch`). A `sleep` lock
//!   wouldn't do: logind ignores those for the lid by default
//!   (`LidSwitchIgnoreInhibited=yes`), and it isn't taken here, so Suspend
//!   from the menu still suspends.
//! - The desktop's own "don't suspend when idle" inhibitor on the session
//!   bus, so its idle timer (GNOME's "Automatic Suspend", KDE's "Sleep
//!   after") doesn't put a closed, idle laptop to sleep a few minutes
//!   later: `org.gnome.SessionManager.Inhibit` with the suspend flag
//!   (GNOME, Cinnamon, MATE) and `org.freedesktop.PowerManagement.Inhibit`
//!   (KDE's PowerDevil, Xfce), whichever the session has. Each lasts as
//!   long as the D-Bus connection that asked. The screen still blanks and
//!   locks as usual. (xdg-desktop-portal's Inhibit would be one call for
//!   every desktop, but on GNOME 46 it answers success and registers
//!   nothing with gnome-session.)
//!
//! The lid lock is the hold; without it `hold` fails. The desktop
//! inhibitors are best effort: a window manager with neither service has no
//! idle timer of theirs to stop.
//!
//! KDE Plasma is the exception. PowerDevil takes logind's lid-switch lock
//! for itself, runs its own lid action, and consults no inhibitor before it
//! (https://bugs.kde.org/show_bug.cgi?id=485623); only its own setting,
//! "When laptop lid closed", decides. So where PowerDevil runs
//! (`org.kde.Solid.PowerManagement` on the session bus) the hold covers idle
//! suspend only and says so ([`Backend::caveat`]).

use std::path::Path;
use std::time::Duration;

use clawdmeter_core::keep_awake::{Backend, Power};
use dbus::arg::OwnedFd;
use dbus::blocking::Connection;

const TIMEOUT: Duration = Duration::from_secs(5);
/// Who and why, as `systemd-inhibit --list` and the desktop show them.
const WHO: &str = "Clawdmeter";
const WHY: &str = "Keep awake with the lid closed";
/// gnome-session's inhibit flag for suspend (1 logout, 2 user switch,
/// 4 suspend, 8 idle: the last would keep the screen on too).
const GSM_SUSPEND: u32 = 4;

/// The two holds, while held.
#[derive(Default)]
pub(super) struct Logind {
    /// logind's lock: held while this is open.
    lid: Option<OwnedFd>,
    /// The session-bus connection the desktop's inhibitors belong to: they
    /// stand while it is open.
    idle: Option<Connection>,
    /// That connection dropped: taken again on the next poll.
    idle_lost: bool,
    /// PowerDevil runs here: the lid is its to decide.
    plasma: bool,
}

/// The menu's words where Plasma decides the lid.
const PLASMA_CAVEAT: &str = "on Plasma, also set the lid to Do nothing";

impl Backend for Logind {
    fn hold(&mut self) -> Result<(), String> {
        self.lid = Some(inhibit_lid()?);
        self.idle_lost = false;
        let (idle, plasma) = inhibit_idle_suspend();
        self.plasma = plasma;
        match idle {
            Ok(conn) => self.idle = Some(conn),
            Err(e) => log::info!("keep awake: no desktop suspend inhibitor ({e}); the lid lock still holds"),
        }
        if self.plasma {
            log::warn!(
                "keep awake: KDE Plasma handles the lid itself and ignores inhibitors \
                 (https://bugs.kde.org/show_bug.cgi?id=485623). Set System Settings > Power Management > \
                 \"When laptop lid closed\" to \"Do nothing\"; idle sleep is held off here."
            );
        }
        Ok(())
    }

    fn caveat(&self) -> Option<String> {
        self.plasma.then(|| PLASMA_CAVEAT.to_owned())
    }

    fn holding(&mut self) -> bool {
        // The lid lock stands while its descriptor is open. The desktop's
        // inhibitors go with their connection: if it drops they are taken
        // again here, and the lid lock is left alone (letting go of it with
        // the lid shut would suspend the laptop before the next one lands).
        if let Some(conn) = &self.idle {
            // Read what arrived, so nothing queues up for as long as it holds.
            let _ = conn.process(Duration::ZERO);
            if !conn.channel().is_connected() {
                log::info!("keep awake: the session bus connection dropped; taking the suspend inhibitor again");
                self.idle = None;
                self.idle_lost = true;
            }
        }
        if self.idle_lost {
            if let (Ok(conn), _) = inhibit_idle_suspend() {
                self.idle = Some(conn);
                self.idle_lost = false;
            }
        }
        self.lid.is_some()
    }

    fn release(&mut self) -> Result<(), String> {
        // Closing the connection and the descriptor is the release.
        self.idle = None;
        self.idle_lost = false;
        self.lid = None;
        Ok(())
    }
}

/// A D-Bus error without the trailing newline libdbus leaves on some.
fn text(e: dbus::Error) -> String {
    e.to_string().trim().to_owned()
}

/// Take logind's lid-switch lock.
fn inhibit_lid() -> Result<OwnedFd, String> {
    let conn = Connection::new_system().map_err(|e| format!("no system bus: {}", text(e)))?;
    let proxy = conn.with_proxy("org.freedesktop.login1", "/org/freedesktop/login1", TIMEOUT);
    let (fd,): (OwnedFd,) = proxy
        .method_call("org.freedesktop.login1.Manager", "Inhibit", ("handle-lid-switch", WHO, WHY, "block"))
        .map_err(|e| format!("logind refused the lid-switch lock: {}", text(e)))?;
    Ok(fd)
}

/// Ask the desktop not to suspend when idle, through each inhibitor service
/// the session has. The connection that comes back holds them. The flag
/// says PowerDevil runs in this session (Plasma), whether or not it took
/// the inhibitor: the lid is its to decide either way.
fn inhibit_idle_suspend() -> (Result<Connection, String>, bool) {
    let conn = match Connection::new_session() {
        Ok(conn) => conn,
        Err(e) => return (Err(format!("no session bus: {}", text(e))), false),
    };
    let bus = conn.with_proxy("org.freedesktop.DBus", "/org/freedesktop/DBus", TIMEOUT);
    let present = |name: &str| {
        bus.method_call::<(bool,), _, _, _>("org.freedesktop.DBus", "NameHasOwner", (name,)).is_ok_and(|(b,)| b)
    };
    let plasma = present("org.kde.Solid.PowerManagement");
    // The cookie each call returns is only for taking the inhibitor back
    // early; closing the connection does that, so it isn't kept (PowerDevil
    // hands it out as an int32, the others as a uint32).
    let mut held = Vec::new();
    let mut failed = Vec::new();
    let mut note = |name: &'static str, result: Result<(), dbus::Error>| match result {
        Ok(()) => held.push(name),
        Err(e) => failed.push(format!("{name}: {}", text(e))),
    };
    if present("org.gnome.SessionManager") {
        let gsm = conn.with_proxy("org.gnome.SessionManager", "/org/gnome/SessionManager", TIMEOUT);
        note(
            "org.gnome.SessionManager",
            gsm.method_call("org.gnome.SessionManager", "Inhibit", (WHO, 0u32, WHY, GSM_SUSPEND)),
        );
    }
    if present("org.freedesktop.PowerManagement") {
        let pm =
            conn.with_proxy("org.freedesktop.PowerManagement", "/org/freedesktop/PowerManagement/Inhibit", TIMEOUT);
        note(
            "org.freedesktop.PowerManagement",
            pm.method_call("org.freedesktop.PowerManagement.Inhibit", "Inhibit", (WHO, WHY)),
        );
    }
    let held = match (held.is_empty(), failed.is_empty()) {
        (false, _) => {
            log::debug!("keep awake: idle suspend inhibited through {}", held.join(", "));
            Ok(conn)
        }
        (true, false) => Err(failed.join("; ")),
        (true, true) => Err("this session has no org.gnome.SessionManager or org.freedesktop.PowerManagement".into()),
    };
    (held, plasma)
}

/// The computer's power, from `/sys/class/power_supply`.
pub(super) fn power() -> Power {
    power_from(Path::new("/sys/class/power_supply"))
}

/// [`power`] from any folder laid out like sysfs's. Supplies with
/// `scope = Device` are a mouse's or a headset's battery, not the
/// computer's. A battery that says it is discharging means on battery,
/// whatever the chargers say: one too weak for the load is no reason to
/// skip the floor. Otherwise external power is a mains or USB supply that
/// is online, or a battery that says it is charging, full or idle.
/// Several batteries count as their average charge.
fn power_from(dir: &Path) -> Power {
    let Ok(entries) = std::fs::read_dir(dir) else { return Power::UNKNOWN };
    let read = |supply: &Path, file: &str| {
        std::fs::read_to_string(supply.join(file)).ok().map(|s| s.trim().to_owned()).filter(|s| !s.is_empty())
    };
    let (mut chargers, mut online) = (0, false);
    let (mut charges, mut discharging, mut fed, mut batteries) = (Vec::new(), false, false, 0);
    for entry in entries.flatten() {
        let supply = entry.path();
        if read(&supply, "scope").is_some_and(|s| s.eq_ignore_ascii_case("device")) {
            continue;
        }
        match read(&supply, "type").as_deref() {
            Some("Battery") => {
                batteries += 1;
                if let Some(pct) = read(&supply, "capacity").and_then(|c| c.parse::<u8>().ok()) {
                    charges.push(pct.min(100));
                }
                let status = read(&supply, "status");
                discharging |= status.as_deref() == Some("Discharging");
                fed |= matches!(status.as_deref(), Some("Charging" | "Full" | "Not charging"));
            }
            Some(t) if t == "Mains" || t.starts_with("USB") => {
                chargers += 1;
                online |= read(&supply, "online").is_some_and(|o| o != "0");
            }
            _ => {}
        }
    }
    let external = match (chargers, batteries) {
        // No battery (a desktop, whatever its USB-C ports list): nothing
        // says it isn't on external power.
        (_, 0) => online.then_some(true),
        _ if discharging => Some(false),
        // A dock that lists as offline, or no charger listed at all, while
        // the battery says it is fed still powers the computer. A battery
        // that says nothing either way counts as on battery.
        _ => Some(online || fed),
    };
    let battery_pct =
        (!charges.is_empty()).then(|| (charges.iter().map(|&c| c as u32).sum::<u32>() / charges.len() as u32) as u8);
    Power { external, battery_pct }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A sysfs-like folder: `(supply, [(file, contents)])`.
    fn sysfs(name: &str, supplies: &[(&str, &[(&str, &str)])]) -> std::path::PathBuf {
        let dir = std::env::temp_dir().join(format!("cm-power-{}-{name}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        for (supply, files) in supplies {
            std::fs::create_dir_all(dir.join(supply)).unwrap();
            for (file, text) in *files {
                std::fs::write(dir.join(supply).join(file), format!("{text}\n")).unwrap();
            }
        }
        dir
    }

    fn check(name: &str, supplies: &[(&str, &[(&str, &str)])], want: Power) {
        let dir = sysfs(name, supplies);
        assert_eq!(power_from(&dir), want, "{name}");
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn reads_the_computers_power() {
        check(
            "plugged",
            &[
                ("AC", &[("type", "Mains"), ("online", "1")]),
                ("BAT0", &[("type", "Battery"), ("capacity", "99"), ("status", "Not charging")]),
                ("ucsi-source-psy-USBC000:001", &[("type", "USB"), ("scope", "System"), ("online", "0")]),
            ],
            Power { external: Some(true), battery_pct: Some(99) },
        );
        check(
            "on-battery",
            &[
                ("AC", &[("type", "Mains"), ("online", "0")]),
                ("BAT0", &[("type", "Battery"), ("capacity", "41"), ("status", "Discharging")]),
            ],
            Power { external: Some(false), battery_pct: Some(41) },
        );
        check(
            "usb-c",
            &[
                ("BAT1", &[("type", "Battery"), ("capacity", "60"), ("status", "Charging")]),
                ("ucsi-source-psy-USBC000:002", &[("type", "USB"), ("online", "1")]),
            ],
            Power { external: Some(true), battery_pct: Some(60) },
        );
        check(
            "no-charger-listed",
            &[("BAT0", &[("type", "Battery"), ("capacity", "30"), ("status", "Discharging")])],
            Power { external: Some(false), battery_pct: Some(30) },
        );
        check(
            "two-batteries",
            &[
                ("AC", &[("type", "Mains"), ("online", "0")]),
                ("BAT0", &[("type", "Battery"), ("capacity", "10"), ("status", "Discharging")]),
                ("BAT1", &[("type", "Battery"), ("capacity", "50"), ("status", "Not charging")]),
            ],
            Power { external: Some(false), battery_pct: Some(30) },
        );
    }

    #[test]
    fn a_draining_battery_is_on_battery_whatever_the_charger_says() {
        check(
            "weak-charger",
            &[
                ("ucsi-source-psy-USBC000:001", &[("type", "USB"), ("online", "1")]),
                ("BAT0", &[("type", "Battery"), ("capacity", "22"), ("status", "Discharging")]),
            ],
            Power { external: Some(false), battery_pct: Some(22) },
        );
        check(
            "dock-listed-offline",
            &[
                ("AC", &[("type", "Mains"), ("online", "0")]),
                ("BAT0", &[("type", "Battery"), ("capacity", "80"), ("status", "Full")]),
            ],
            Power { external: Some(true), battery_pct: Some(80) },
        );
        check(
            "no-word-either-way",
            &[
                ("AC", &[("type", "Mains"), ("online", "0")]),
                ("BAT0", &[("type", "Battery"), ("capacity", "15"), ("status", "Unknown")]),
            ],
            Power { external: Some(false), battery_pct: Some(15) },
        );
        check(
            "no-charger-listed-charging",
            &[("BAT0", &[("type", "Battery"), ("capacity", "30"), ("status", "Charging")])],
            Power { external: Some(true), battery_pct: Some(30) },
        );
    }

    #[test]
    fn a_desktop_and_a_mouse_are_not_a_laptop_battery() {
        check("desktop", &[], Power::UNKNOWN);
        check(
            "desktop-with-an-empty-usb-c-port",
            &[("ucsi-source-psy-USBC000:001", &[("type", "USB"), ("online", "0")])],
            Power::UNKNOWN,
        );
        assert_eq!(power_from(Path::new("/nonexistent/power_supply")), Power::UNKNOWN);
        check(
            "mouse",
            &[(
                "hidpp_battery_0",
                &[("type", "Battery"), ("scope", "Device"), ("capacity", "5"), ("status", "Discharging")],
            )],
            Power::UNKNOWN,
        );
        check(
            "desktop-on-mains",
            &[("AC", &[("type", "Mains"), ("online", "1")])],
            Power { external: Some(true), battery_pct: None },
        );
        check(
            "unreadable-charge",
            &[
                ("AC", &[("type", "Mains"), ("online", "0")]),
                ("BAT0", &[("type", "Battery"), ("status", "Discharging")]),
            ],
            Power { external: Some(false), battery_pct: None },
        );
    }
}
