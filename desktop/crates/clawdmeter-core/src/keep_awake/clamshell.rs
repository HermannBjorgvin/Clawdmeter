//! macOS: keep awake with the lid closed through the kernel's lid flag.
//!
//! No public power assertion stops the sleep a closed lid starts. The power
//! root domain has a flag that does ("the lid doesn't sleep this Mac"), and
//! any process may set it: no admin rights, no helper, nothing written to
//! disk. [`LidHold`] sets it while holding.
//!
//! The flag is not tied to the process that set it: it stays until someone
//! clears it or the Mac restarts. So a marker goes into the [`RestoreFile`]
//! before it is set, and the next start clears a flag a crashed run left.
//! `powerd` uses the same flag and may clear it (a change of power source
//! does), so the hold sets it again on every poll ([`LidHold::renew`]).
//!
//! The flag has no owner: clearing it also clears it for any other app that
//! set it. And macOS acts on it the moment it is cleared, so letting go with
//! the lid shut sleeps the Mac at once, which is what a release is for.
//!
//! The IOKit call is the app crate's [`Lid`]; this half is the logic, which
//! the tests run over a fake.

use serde::{Deserialize, Serialize};

use super::{Power, RestoreFile};

/// The kernel's lid flag.
pub trait Lid {
    /// Whether closing the lid may sleep the Mac (`false`: it may, the
    /// default).
    fn set_sleep_disabled(&mut self, disabled: bool) -> Result<(), String>;
}

/// The restore record: this app set the flag and hasn't cleared it yet.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
struct Mark {
    lid_sleep_disabled: bool,
}

/// Holds the lid flag through a [`Lid`].
pub struct LidHold<L: Lid> {
    lid: L,
    file: RestoreFile,
}

impl<L: Lid> LidHold<L> {
    /// A hold over `lid`, with its marker in `file`. Touches nothing.
    pub fn new(lid: L, file: RestoreFile) -> Self {
        LidHold { lid, file }
    }

    /// Set the flag. A flag an earlier run left set is simply taken over.
    /// `Err` leaves it clear.
    pub fn hold(&mut self) -> Result<(), String> {
        // The marker first: a crash from here on is healed by the next start.
        self.file
            .save(&Mark { lid_sleep_disabled: true })
            .map_err(|e| format!("can't save {}: {e}", self.file.path().display()))?;
        if let Err(e) = self.lid.set_sleep_disabled(true) {
            if let Err(undo) = self.restore() {
                log::error!("keep awake: couldn't clear the lid flag ({undo}); trying again later");
            }
            return Err(e);
        }
        Ok(())
    }

    /// Set the flag again, in case `powerd` cleared it. `false`: it couldn't
    /// be set, and the hold is lost. The marker is written again first if
    /// it has gone, so a set flag is never without one.
    pub fn renew(&mut self) -> bool {
        if !self.file.path().exists() && self.file.save(&Mark { lid_sleep_disabled: true }).is_err() {
            return false;
        }
        self.lid.set_sleep_disabled(true).is_ok()
    }

    /// Clear the flag if the marker says this app set it, and drop the
    /// marker. A marker that can't be read counts as set: clear is the
    /// flag's default, so clearing is always the safe side. `Err` keeps the
    /// marker for another try.
    pub fn restore(&mut self) -> Result<(), String> {
        if matches!(self.file.load::<Mark>(), Ok(None)) {
            return Ok(());
        }
        self.lid.set_sleep_disabled(false)?;
        self.file.clear().map_err(|e| format!("can't remove {}: {e}", self.file.path().display()))
    }
}

/// The Mac's power from `pmset -g batt`:
///
/// ```text
/// Now drawing from 'Battery Power'
///  -InternalBattery-0 (id=1234567)  64%; discharging; 4:12 remaining present: true
/// ```
///
/// A desktop has the first line alone. A UPS reads as a battery. A battery
/// that says it is discharging counts as on battery whatever the first line
/// says: a charger too weak for the load is no reason to skip the floor.
pub fn power_from_pmset(text: &str) -> Power {
    let draining = text
        .lines()
        .any(|line| line.trim_start().starts_with('-') && line.split(';').any(|field| field.trim() == "discharging"));
    let external = text.lines().find_map(|line| {
        let source = line.trim().strip_prefix("Now drawing from '")?;
        Some(source.starts_with("AC Power") && !draining)
    });
    let pct = |line: &str| -> Option<u8> {
        let end = line.find('%')?;
        let digits = line[..end].bytes().rev().take_while(u8::is_ascii_digit).count();
        line[end - digits..end].parse::<u8>().ok().map(|p| p.min(100))
    };
    let battery_pct = text
        .lines()
        .filter(|line| line.contains("InternalBattery"))
        .find_map(pct)
        .or_else(|| text.lines().filter(|line| line.trim_start().starts_with('-')).find_map(pct));
    Power { external, battery_pct }
}

#[cfg(test)]
mod tests {
    use std::cell::RefCell;
    use std::rc::Rc;

    use super::*;

    /// The flag in memory: nothing of macOS is touched.
    #[derive(Clone, Default)]
    struct Fake {
        flag: Rc<RefCell<bool>>,
        calls: Rc<RefCell<Vec<bool>>>,
        fail: Rc<RefCell<bool>>,
    }

    impl Lid for Fake {
        fn set_sleep_disabled(&mut self, disabled: bool) -> Result<(), String> {
            if *self.fail.borrow() {
                return Err("not permitted".into());
            }
            self.calls.borrow_mut().push(disabled);
            *self.flag.borrow_mut() = disabled;
            Ok(())
        }
    }

    fn scratch(name: &str) -> RestoreFile {
        let dir = std::env::temp_dir().join(format!("cm-keep-awake-lid-{name}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        RestoreFile::in_dir(&dir)
    }

    #[test]
    fn a_hold_sets_the_flag_and_a_restore_clears_it() {
        let (fake, file) = (Fake::default(), scratch("roundtrip"));
        let mut hold = LidHold::new(fake.clone(), file.clone());
        hold.restore().unwrap();
        assert!(fake.calls.borrow().is_empty(), "with no marker the flag isn't this app's to clear");
        hold.hold().unwrap();
        assert!(*fake.flag.borrow());
        assert!(file.path().exists(), "marked on disk for the next start");
        assert!(hold.renew());
        assert_eq!(*fake.calls.borrow(), [true, true], "set again on a poll");
        hold.restore().unwrap();
        assert!(!*fake.flag.borrow());
        assert!(!file.path().exists());
    }

    #[test]
    fn a_renewed_flag_is_never_without_its_marker() {
        let (fake, file) = (Fake::default(), scratch("renew"));
        let mut hold = LidHold::new(fake.clone(), file.clone());
        hold.hold().unwrap();
        // Something else removed the marker (a second instance letting go).
        file.clear().unwrap();
        assert!(hold.renew());
        assert!(file.path().exists(), "a crash now is still healed by the next start");
    }

    #[test]
    fn the_next_start_clears_a_flag_a_crash_left() {
        let (fake, file) = (Fake::default(), scratch("crash"));
        LidHold::new(fake.clone(), file.clone()).hold().unwrap();
        // The process dies here: the kernel keeps the flag. A new one starts.
        assert!(*fake.flag.borrow());
        LidHold::new(fake.clone(), file.clone()).restore().unwrap();
        assert!(!*fake.flag.borrow());
        assert!(!file.path().exists());
    }

    #[test]
    fn a_start_that_holds_takes_the_flag_over_without_clearing_it() {
        let (fake, file) = (Fake::default(), scratch("takeover"));
        LidHold::new(fake.clone(), file.clone()).hold().unwrap();
        LidHold::new(fake.clone(), file).hold().unwrap();
        assert_eq!(*fake.calls.borrow(), [true, true], "never cleared in between: a shut lid would sleep the Mac");
    }

    #[test]
    fn a_refused_call_holds_nothing_and_a_failed_clear_keeps_the_marker() {
        let (fake, file) = (Fake::default(), scratch("refused"));
        let mut hold = LidHold::new(fake.clone(), file.clone());
        *fake.fail.borrow_mut() = true;
        assert_eq!(hold.hold(), Err("not permitted".into()));
        assert!(file.path().exists(), "the clear failed too, so the marker waits for another try");
        assert!(!hold.renew());
        *fake.fail.borrow_mut() = false;
        hold.restore().unwrap();
        assert!(!file.path().exists());
    }

    #[test]
    fn a_marker_that_cant_be_read_still_clears_the_flag() {
        let (fake, file) = (Fake::default(), scratch("unreadable"));
        *fake.flag.borrow_mut() = true;
        std::fs::create_dir_all(file.path().parent().unwrap()).unwrap();
        std::fs::write(file.path(), b"{").unwrap();
        LidHold::new(fake.clone(), file.clone()).restore().unwrap();
        assert!(!*fake.flag.borrow());
        assert!(!file.path().exists());
    }

    #[test]
    fn power_from_pmset_output() {
        let plugged = "Now drawing from 'AC Power'\n -InternalBattery-0 (id=23855203)\t100%; charged; 0:00 remaining present: true\n";
        assert_eq!(power_from_pmset(plugged), Power { external: Some(true), battery_pct: Some(100) });
        let battery = "Now drawing from 'Battery Power'\n -InternalBattery-0 (id=23855203)\t64%; discharging; 4:12 remaining present: true\n";
        assert_eq!(power_from_pmset(battery), Power { external: Some(false), battery_pct: Some(64) });
        let low = "Now drawing from 'Battery Power'\n -InternalBattery-0 (id=1)\t7%; discharging; (no estimate) present: true\n";
        assert_eq!(power_from_pmset(low).battery_pct, Some(7));
        assert_eq!(
            power_from_pmset("Now drawing from 'AC Power'\n"),
            Power { external: Some(true), battery_pct: None },
            "a desktop"
        );
        let ups =
            "Now drawing from 'UPS Power'\n -Back-UPS ES 700 (id=1)\t83%; discharging; 0:40 remaining present: true\n";
        assert_eq!(
            power_from_pmset(ups),
            Power { external: Some(false), battery_pct: Some(83) },
            "a desktop on its UPS"
        );
        let both = "Now drawing from 'Battery Power'\n -UPS (id=2)\t90%; charged present: true\n -InternalBattery-0 (id=1)\t41%; discharging present: true\n";
        assert_eq!(power_from_pmset(both).battery_pct, Some(41), "the laptop's own battery, not the UPS");
        let weak =
            "Now drawing from 'AC Power'\n -InternalBattery-0 (id=1)\t31%; discharging; 1:02 remaining present: true\n";
        assert_eq!(
            power_from_pmset(weak),
            Power { external: Some(false), battery_pct: Some(31) },
            "a charger that can't keep up: the floor applies"
        );
        let idle =
            "Now drawing from 'AC Power'\n -InternalBattery-0 (id=1)\t80%; AC attached; not charging present: true\n";
        assert_eq!(power_from_pmset(idle).external, Some(true));
        assert_eq!(power_from_pmset(""), Power::UNKNOWN);
        assert_eq!(power_from_pmset("pmset: command not found"), Power::UNKNOWN);
        assert_eq!(
            power_from_pmset("Now drawing from 'Battery Power'\n -InternalBattery-0 (id=1)\t%; present: true\n"),
            Power { external: Some(false), battery_pct: None }
        );
    }
}
