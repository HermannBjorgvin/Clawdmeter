//! Keep the computer awake with the lid closed (`keep_awake`, the tray's
//! checkbox): when to hold, when to let go, and what to put back.
//!
//! This is the half with no OS calls. A [`Backend`] does the holding (the
//! app crate has one per OS); [`Keeper`] decides when, from the setting and
//! the computer's [`Power`]:
//!
//! - On external power it holds for as long as the setting is on.
//! - On battery it holds while the charge is above [`BATTERY_FLOOR`], lets
//!   go at the floor so the computer sleeps like any other with its lid
//!   closed, and holds again [`BATTERY_RESUME`] points higher or once it is
//!   plugged in. A laptop left awake in a bag stops at the floor rather than
//!   at empty.
//! - On battery with no reading of the charge it doesn't hold: it can't tell
//!   how long is left.
//! - With no battery to read (a desktop) it holds.
//!
//! A hold the OS drops when the process dies needs no undoing. One that
//! changes a setting which outlives the process is written to a
//! [`RestoreFile`] before the change, so the next start puts the old value
//! back after a crash ([`Backend::heal`]).

use std::fs;
use std::io;
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};

/// On battery, the charge (%) at which the hold is let go.
pub const BATTERY_FLOOR: u8 = 20;
/// How far above the floor the charge has to be before it holds again, so a
/// reading that hovers at the floor doesn't flap.
pub const BATTERY_RESUME: u8 = 5;

/// A failed hold is tried again after these waits, the last one from then on.
const RETRY_SECS: [i64; 4] = [10, 30, 60, 300];

/// Where the computer's power comes from, as far as the OS says.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct Power {
    /// On external power. `None`: unknown.
    pub external: Option<bool>,
    /// The battery's charge, 0–100. `None`: no battery, or unreadable.
    pub battery_pct: Option<u8>,
}

impl Power {
    /// A computer with no battery, or one whose power can't be read.
    pub const UNKNOWN: Power = Power { external: None, battery_pct: None };
}

/// Why the hold is let go although the setting is on.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Pause {
    /// On battery at or under the floor (the charge, %).
    BatteryLow(u8),
    /// On battery and the charge can't be read.
    BatteryUnknown,
}

/// What the keeper is doing, for the menu and the log.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub enum Status {
    /// The setting is off.
    #[default]
    Off,
    /// Holding: the computer stays awake with the lid closed.
    Holding,
    /// Holding what it can, and the lid is out of its hands (what the user
    /// has to do, in a few words: [`Backend::caveat`]).
    Limited(String),
    /// The setting is on and the hold is let go, for the computer's sake.
    Paused(Pause),
    /// The setting is on and the OS wouldn't hold (the reason). Tried again
    /// later.
    Failed(String),
}

impl Status {
    /// The words the menu adds after the item's name, when there is
    /// something to say: "paused, battery at 18%".
    pub fn note(&self) -> Option<String> {
        match self {
            Status::Off | Status::Holding => None,
            Status::Limited(what) => Some(what.clone()),
            Status::Paused(Pause::BatteryLow(pct)) => Some(format!("paused, battery at {pct}%")),
            Status::Paused(Pause::BatteryUnknown) => Some("paused on battery".into()),
            Status::Failed(_) => Some("unavailable".into()),
        }
    }
}

/// Whether to hold on this power, given whether it holds now (the resume
/// margin applies only to a hold that was let go).
pub fn verdict(power: Power, holding: bool) -> Result<(), Pause> {
    match (power.external, power.battery_pct) {
        (Some(true), _) | (None, None) => Ok(()),
        (Some(false), None) => Err(Pause::BatteryUnknown),
        // A charge with no word on the charger is read as on battery: the
        // floor is the careful side.
        (_, Some(pct)) => {
            let need = if holding { BATTERY_FLOOR } else { BATTERY_FLOOR.saturating_add(BATTERY_RESUME) };
            if (holding && pct > need) || (!holding && pct >= need) {
                Ok(())
            } else {
                Err(Pause::BatteryLow(pct))
            }
        }
    }
}

/// What keeps the computer awake on one OS.
pub trait Backend {
    /// Put back whatever a run that never released left changed. Called
    /// once, before anything else.
    fn heal(&mut self) {}

    /// Keep the computer awake with the lid closed, until [`release`]. Only
    /// called while not holding. `Err` holds nothing: a half-taken hold is
    /// released before it returns.
    ///
    /// [`release`]: Backend::release
    fn hold(&mut self) -> Result<(), String>;

    /// What this hold doesn't cover, when the desktop keeps the lid to
    /// itself: a few words for the menu on what the user has to set. Only
    /// called while holding.
    fn caveat(&self) -> Option<String> {
        None
    }

    /// Whether the hold still stands (a lock can be lost with its
    /// connection, a setting changed by someone else). Only called while
    /// holding.
    fn holding(&mut self) -> bool {
        true
    }

    /// Let go and leave the OS as it was before [`hold`]. Called when
    /// holding, and safe when not.
    ///
    /// [`hold`]: Backend::hold
    fn release(&mut self);
}

/// Holds and lets go of a [`Backend`] as the setting and the power say.
pub struct Keeper<B: Backend> {
    backend: B,
    holding: bool,
    status: Status,
    /// Failed holds in a row, and when to try again.
    strikes: usize,
    retry_at: i64,
}

impl<B: Backend> Keeper<B> {
    /// A keeper over `backend`, after healing what an earlier run left.
    pub fn new(mut backend: B) -> Self {
        backend.heal();
        Keeper { backend, holding: false, status: Status::Off, strikes: 0, retry_at: 0 }
    }

    /// What it's doing now.
    pub fn status(&self) -> &Status {
        &self.status
    }

    /// Bring the hold in line with the setting (`wanted`) and the power, at
    /// unix time `now`. Returns the status, which changes only here.
    pub fn tick(&mut self, wanted: bool, power: Power, now: i64) -> &Status {
        if !wanted {
            self.let_go();
            self.strikes = 0;
            self.retry_at = 0;
            return self.set(Status::Off);
        }
        if let Err(pause) = verdict(power, self.holding) {
            self.let_go();
            return self.set(Status::Paused(pause));
        }
        if self.holding && !self.backend.holding() {
            log::warn!("keep awake: the hold was lost; taking it again");
            // Whatever is left of it goes before the next attempt.
            self.let_go();
        }
        if !self.holding && now >= self.retry_at {
            match self.backend.hold() {
                Ok(()) => {
                    self.holding = true;
                    self.strikes = 0;
                    self.retry_at = 0;
                }
                Err(e) => {
                    let wait = RETRY_SECS[self.strikes.min(RETRY_SECS.len() - 1)];
                    self.strikes += 1;
                    self.retry_at = now + wait;
                    return self.set(Status::Failed(e));
                }
            }
        }
        if self.holding {
            let status = self.backend.caveat().map_or(Status::Holding, Status::Limited);
            self.set(status)
        } else {
            // Waiting out a retry: the last failure still stands.
            &self.status
        }
    }

    /// Let go for good (quit). The setting is untouched.
    pub fn stop(&mut self) {
        self.let_go();
        self.set(Status::Off);
    }

    fn let_go(&mut self) {
        if self.holding {
            self.backend.release();
            self.holding = false;
        }
    }

    fn set(&mut self, status: Status) -> &Status {
        if self.status != status {
            match &status {
                Status::Off => log::info!("keep awake: off"),
                Status::Holding => log::info!("keep awake: holding (the computer stays awake with the lid closed)"),
                Status::Limited(what) => log::warn!("keep awake: holding, but not the lid ({what})"),
                Status::Paused(Pause::BatteryLow(pct)) => log::info!(
                    "keep awake: paused, battery at {pct}% (holds again at {}% or on external power)",
                    BATTERY_FLOOR.saturating_add(BATTERY_RESUME)
                ),
                Status::Paused(Pause::BatteryUnknown) => {
                    log::info!("keep awake: paused, on battery with no charge reading")
                }
                Status::Failed(e) => log::warn!("keep awake: unavailable ({e}); trying again later"),
            }
            self.status = status;
        }
        &self.status
    }
}

impl<B: Backend> Drop for Keeper<B> {
    fn drop(&mut self) {
        self.let_go();
    }
}

/// The setting a backend changed and has to put back, kept in the state
/// folder from before the change until after the restore, so a run that
/// died in between is healed by the next one. `T` is the backend's own
/// record of the old value.
#[derive(Debug, Clone)]
pub struct RestoreFile {
    path: PathBuf,
}

impl RestoreFile {
    /// The record at `<state_dir>/keep-awake-restore.json`.
    pub fn in_dir(state_dir: &Path) -> Self {
        RestoreFile { path: state_dir.join("keep-awake-restore.json") }
    }

    /// Where it is.
    pub fn path(&self) -> &Path {
        &self.path
    }

    /// Save the old value, replacing any record. Written beside the file and
    /// renamed over it, so a crash leaves the old record or the new one.
    pub fn save<T: Serialize>(&self, original: &T) -> io::Result<()> {
        if let Some(dir) = self.path.parent() {
            fs::create_dir_all(dir)?;
        }
        let tmp = self.path.with_extension("json.tmp");
        fs::write(&tmp, serde_json::to_vec_pretty(original).map_err(io::Error::other)?)?;
        fs::rename(&tmp, &self.path)
    }

    /// The saved value: `Ok(None)` with no record (nothing to put back). A
    /// record that can't be read or parsed is an error, and stays.
    pub fn load<T: for<'de> Deserialize<'de>>(&self) -> io::Result<Option<T>> {
        match fs::read(&self.path) {
            Ok(bytes) => serde_json::from_slice(&bytes).map(Some).map_err(io::Error::other),
            Err(e) if e.kind() == io::ErrorKind::NotFound => Ok(None),
            Err(e) => Err(e),
        }
    }

    /// Drop the record, once the old value is back.
    pub fn clear(&self) -> io::Result<()> {
        match fs::remove_file(&self.path) {
            Err(e) if e.kind() != io::ErrorKind::NotFound => Err(e),
            _ => Ok(()),
        }
    }
}

#[cfg(test)]
mod tests {
    use std::cell::RefCell;
    use std::rc::Rc;

    use super::*;

    /// A backend that records its calls and fails or loses its hold on cue.
    #[derive(Default)]
    struct Fake {
        log: Rc<RefCell<Vec<&'static str>>>,
        fail: Rc<RefCell<bool>>,
        lost: Rc<RefCell<bool>>,
        caveat: Option<&'static str>,
    }

    impl Backend for Fake {
        fn heal(&mut self) {
            self.log.borrow_mut().push("heal");
        }
        fn hold(&mut self) -> Result<(), String> {
            if *self.fail.borrow() {
                self.log.borrow_mut().push("hold-failed");
                return Err("refused".into());
            }
            self.log.borrow_mut().push("hold");
            Ok(())
        }
        fn caveat(&self) -> Option<String> {
            self.caveat.map(str::to_owned)
        }
        fn holding(&mut self) -> bool {
            !std::mem::take(&mut *self.lost.borrow_mut())
        }
        fn release(&mut self) {
            self.log.borrow_mut().push("release");
        }
    }

    type Shared<T> = Rc<RefCell<T>>;
    type Calls = Shared<Vec<&'static str>>;

    fn keeper() -> (Keeper<Fake>, Calls, Shared<bool>, Shared<bool>) {
        let fake = Fake::default();
        let (log, fail, lost) = (fake.log.clone(), fake.fail.clone(), fake.lost.clone());
        (Keeper::new(fake), log, fail, lost)
    }

    const AC: Power = Power { external: Some(true), battery_pct: Some(50) };
    fn battery(pct: u8) -> Power {
        Power { external: Some(false), battery_pct: Some(pct) }
    }

    #[test]
    fn holds_while_on_and_releases_when_off() {
        let (mut k, log, ..) = keeper();
        assert_eq!(*log.borrow(), ["heal"], "an earlier run's leftovers go first");
        assert_eq!(k.tick(false, AC, 0), &Status::Off);
        assert_eq!(k.tick(true, AC, 1), &Status::Holding);
        assert_eq!(k.tick(true, AC, 2), &Status::Holding);
        assert_eq!(*log.borrow(), ["heal", "hold"], "held once, not once per tick");
        assert_eq!(k.tick(false, AC, 3), &Status::Off);
        assert_eq!(*log.borrow(), ["heal", "hold", "release"]);
        assert_eq!(k.tick(false, AC, 4), &Status::Off);
        assert_eq!(log.borrow().len(), 3, "nothing to release twice");
    }

    #[test]
    fn quitting_and_dropping_release() {
        let (mut k, log, ..) = keeper();
        k.tick(true, AC, 0);
        k.stop();
        assert_eq!(*log.borrow(), ["heal", "hold", "release"]);
        assert_eq!(k.status(), &Status::Off);
        drop(k);
        assert_eq!(log.borrow().len(), 3, "already released");

        let (mut k, log, ..) = keeper();
        k.tick(true, AC, 0);
        drop(k);
        assert_eq!(*log.borrow(), ["heal", "hold", "release"], "a keeper dropped while holding lets go");
    }

    #[test]
    fn the_battery_floor_pauses_and_the_margin_resumes() {
        let (mut k, log, ..) = keeper();
        assert_eq!(k.tick(true, battery(60), 0), &Status::Holding, "on battery above the floor");
        assert_eq!(k.tick(true, battery(BATTERY_FLOOR + 1), 1), &Status::Holding);
        assert_eq!(k.tick(true, battery(BATTERY_FLOOR), 2), &Status::Paused(Pause::BatteryLow(BATTERY_FLOOR)));
        assert_eq!(*log.borrow(), ["heal", "hold", "release"], "at the floor the computer may sleep");
        let nearly = BATTERY_FLOOR + BATTERY_RESUME - 1;
        assert_eq!(k.tick(true, battery(nearly), 3), &Status::Paused(Pause::BatteryLow(nearly)), "no flapping");
        assert_eq!(k.tick(true, battery(BATTERY_FLOOR + BATTERY_RESUME), 4), &Status::Holding);
        assert_eq!(k.tick(true, battery(5), 5), &Status::Paused(Pause::BatteryLow(5)));
        assert_eq!(
            k.tick(true, Power { external: Some(true), battery_pct: Some(5) }, 6),
            &Status::Holding,
            "plugged in"
        );
        assert_eq!(*log.borrow(), ["heal", "hold", "release", "hold", "release", "hold"]);
    }

    #[test]
    fn verdicts_by_power() {
        assert_eq!(verdict(Power::UNKNOWN, false), Ok(()), "a desktop has no battery to mind");
        assert_eq!(verdict(Power { external: Some(true), battery_pct: None }, false), Ok(()));
        assert_eq!(verdict(Power { external: Some(true), battery_pct: Some(1) }, false), Ok(()));
        assert_eq!(
            verdict(Power { external: Some(false), battery_pct: None }, true),
            Err(Pause::BatteryUnknown),
            "on battery with no reading: no telling how long is left"
        );
        let unplugged_unknown = Power { external: None, battery_pct: Some(10) };
        assert_eq!(verdict(unplugged_unknown, true), Err(Pause::BatteryLow(10)), "no word on the charger: the floor");
        assert_eq!(verdict(Power { external: None, battery_pct: Some(90) }, false), Ok(()));
        assert_eq!(verdict(battery(100), false), Ok(()));
        assert_eq!(verdict(battery(0), true), Err(Pause::BatteryLow(0)));
    }

    #[test]
    fn a_refused_hold_is_tried_again_later() {
        let (mut k, log, fail, _) = keeper();
        *fail.borrow_mut() = true;
        assert_eq!(k.tick(true, AC, 100), &Status::Failed("refused".into()));
        assert_eq!(k.tick(true, AC, 101), &Status::Failed("refused".into()), "still failed while waiting");
        assert_eq!(*log.borrow(), ["heal", "hold-failed"], "not hammered every tick");
        k.tick(true, AC, 100 + RETRY_SECS[0]);
        assert_eq!(log.borrow().len(), 3, "the first retry");
        k.tick(true, AC, 100 + RETRY_SECS[0] + RETRY_SECS[1] - 1);
        assert_eq!(log.borrow().len(), 3, "the waits grow");
        *fail.borrow_mut() = false;
        assert_eq!(k.tick(true, AC, 100 + RETRY_SECS[0] + RETRY_SECS[1]), &Status::Holding);
        // Switching off and on again starts fresh, with no wait.
        *fail.borrow_mut() = true;
        k.tick(false, AC, 1000);
        k.tick(true, AC, 1001);
        k.tick(false, AC, 1002);
        *fail.borrow_mut() = false;
        assert_eq!(k.tick(true, AC, 1003), &Status::Holding, "a new tick of the box tries at once");
    }

    #[test]
    fn a_lost_hold_is_taken_again() {
        let (mut k, log, _, lost) = keeper();
        k.tick(true, AC, 0);
        *lost.borrow_mut() = true;
        assert_eq!(k.tick(true, AC, 1), &Status::Holding);
        assert_eq!(*log.borrow(), ["heal", "hold", "release", "hold"], "what was left is released first");
    }

    #[test]
    fn a_desktop_that_keeps_the_lid_shows_as_limited() {
        let mut k = Keeper::new(Fake { caveat: Some("set the lid by hand"), ..Fake::default() });
        assert_eq!(k.tick(true, AC, 0), &Status::Limited("set the lid by hand".into()));
        assert_eq!(k.status().note().as_deref(), Some("set the lid by hand"));
        assert_eq!(k.tick(false, AC, 1), &Status::Off);
    }

    #[test]
    fn status_notes() {
        assert_eq!(Status::Off.note(), None);
        assert_eq!(Status::Holding.note(), None);
        assert_eq!(Status::Paused(Pause::BatteryLow(18)).note().as_deref(), Some("paused, battery at 18%"));
        assert_eq!(Status::Paused(Pause::BatteryUnknown).note().as_deref(), Some("paused on battery"));
        assert_eq!(Status::Failed("x".into()).note().as_deref(), Some("unavailable"));
    }

    #[test]
    fn the_restore_file_outlives_a_crash_and_clears() {
        let dir = std::env::temp_dir().join(format!("cm-keep-awake-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        let file = RestoreFile::in_dir(&dir.join("state"));
        assert_eq!(file.load::<Vec<u32>>().unwrap(), None, "nothing to put back");
        file.clear().unwrap();
        file.save(&vec![1u32, 3]).unwrap();
        // Another run (a new handle on the same folder) finds it.
        let again = RestoreFile::in_dir(&dir.join("state"));
        assert_eq!(again.load::<Vec<u32>>().unwrap(), Some(vec![1, 3]));
        again.save(&vec![2u32]).unwrap();
        assert_eq!(file.load::<Vec<u32>>().unwrap(), Some(vec![2]), "a new record replaces the old");
        fs::write(file.path(), b"not json").unwrap();
        assert!(file.load::<Vec<u32>>().is_err(), "a record that can't be read is not 'nothing to put back'");
        assert!(file.path().exists());
        file.clear().unwrap();
        assert_eq!(file.load::<Vec<u32>>().unwrap(), None);
        let _ = fs::remove_dir_all(&dir);
    }
}
