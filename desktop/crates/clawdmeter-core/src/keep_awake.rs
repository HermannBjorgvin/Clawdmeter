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
//! back after a crash ([`Backend::heal`]). The two that do: [`plan`]
//! (Windows, the power plan's lid action) and [`clamshell`] (macOS, the
//! kernel's lid flag).

pub mod clamshell;
pub mod plan;

use std::fs;
use std::io::{self, Write};
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

use serde::{Deserialize, Serialize};

/// On battery, the charge (%) at which the hold is let go.
pub const BATTERY_FLOOR: u8 = 20;
/// How far above the floor the charge has to be before it holds again, so a
/// reading that hovers at the floor doesn't flap.
pub const BATTERY_RESUME: u8 = 5;

/// The menu's words for a hold whose checks have failed [`PATIENCE`] times
/// in a row.
const UNCONFIRMED: &str = "can't confirm it is holding";

/// A failed hold is tried again after these waits, the last one from then on.
const RETRY_SECS: [u64; 4] = [10, 30, 60, 300];

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
    /// On battery with no reading of the charge: nothing is held on it.
    pub const UNREAD: Power = Power { external: Some(false), battery_pct: None };
}

/// How many polls in a row may go without an answer (a power reading, a
/// check of the hold) before the keeper stops taking the last answer for
/// granted: about a minute at the thread's 15 s poll.
pub const PATIENCE: u32 = 4;

/// The power readings, poll by poll. One failed reading says nothing about
/// the battery, so the last good one stands in for it: pausing over it
/// would let go of the hold, and with the lid shut that sleeps the
/// computer. After [`PATIENCE`] failures in a row, or with no reading yet,
/// the power counts as [`Power::UNREAD`], which pauses: a battery nobody
/// can read is not held on for long.
#[derive(Debug, Default)]
pub struct Readings {
    last: Option<Power>,
    misses: u32,
}

impl Readings {
    /// The power to act on, given this poll's reading (`None`: it failed).
    pub fn settle(&mut self, reading: Option<Power>) -> Power {
        match reading {
            Some(power) => {
                self.last = Some(power);
                self.misses = 0;
                power
            }
            None => {
                self.misses = self.misses.saturating_add(1);
                match self.last {
                    Some(power) if self.misses < PATIENCE => power,
                    _ => Power::UNREAD,
                }
            }
        }
    }
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

/// What a look at a standing hold found ([`Backend::check`]).
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Check {
    /// The hold stands.
    Standing,
    /// The hold is gone (a lock lost with its connection, another power
    /// plan in use): what is left of it is released and it is taken again.
    Lost,
    /// The look itself failed (the reason), which says nothing about the
    /// hold: it is kept, and looked at again on the next tick. Letting go
    /// over a failed look would end the very hold it was meant to keep, and
    /// with the lid shut that sleeps the computer.
    Unknown(String),
}

/// What keeps the computer awake on one OS.
pub trait Backend {
    /// Put back whatever a run that never released left changed. Called at
    /// most once, on the first tick, and only when that tick doesn't hold:
    /// a start that holds goes straight to [`hold`], so a lid that is shut
    /// then isn't let go of in between.
    ///
    /// `Err`: something is still changed, and the keeper goes on to
    /// [`release`] on its next ticks until that is `Ok`.
    ///
    /// [`hold`]: Backend::hold
    /// [`release`]: Backend::release
    fn heal(&mut self) -> Result<(), String> {
        Ok(())
    }

    /// [`heal`](Backend::heal) for the guard process of the run named
    /// `run`: put back what that run left, and nothing a later run has
    /// changed since. Whether the change is that run's is decided on the
    /// same record the restore then acts on.
    fn heal_for(&mut self, _run: &str) -> Result<(), String> {
        Ok(())
    }

    /// Keep the computer awake with the lid closed, until [`release`]. Only
    /// called while not holding, and it may be the first call of all: what
    /// an earlier run left is its to take over or put back. `Err` holds
    /// nothing: a half-taken hold is released before it returns.
    ///
    /// [`release`]: Backend::release
    fn hold(&mut self) -> Result<(), String>;

    /// What this hold doesn't cover, when the lid is out of its hands (the
    /// desktop keeps it to itself, or someone else changed the setting the
    /// hold made): a few words for the menu. Only called while holding,
    /// after [`check`](Backend::check).
    fn caveat(&self) -> Option<String> {
        None
    }

    /// Whether the hold still stands. Only called while holding.
    fn check(&mut self) -> Check {
        Check::Standing
    }

    /// Let go and leave the OS as it was before [`hold`]. Called when
    /// holding, and safe when not. `Err`: something is still changed, and
    /// the keeper calls again on its next ticks until it is `Ok`.
    ///
    /// [`hold`]: Backend::hold
    fn release(&mut self) -> Result<(), String>;
}

/// Holds and lets go of a [`Backend`] as the setting and the power say.
pub struct Keeper<B: Backend> {
    backend: B,
    holding: bool,
    /// Whether an earlier run's leftovers are dealt with: healed, or taken
    /// over by a hold.
    settled: bool,
    /// A release is owed: one failed, or a hold failed part-way. Tried on
    /// every tick that doesn't hold.
    owed: bool,
    /// The failed release was logged, so it isn't logged every tick.
    complained: bool,
    /// Checks of the hold that failed in a row.
    unsure: u32,
    status: Status,
    /// Failed holds in a row, and when to try again: on the monotonic
    /// clock, so a step of the system clock neither stalls nor rushes it.
    strikes: usize,
    retry_at: Option<Instant>,
}

impl<B: Backend> Keeper<B> {
    /// A keeper over `backend`. Nothing of the OS is touched before the
    /// first [`tick`](Keeper::tick).
    pub fn new(backend: B) -> Self {
        Keeper {
            backend,
            holding: false,
            settled: false,
            owed: false,
            complained: false,
            unsure: 0,
            status: Status::Off,
            strikes: 0,
            retry_at: None,
        }
    }

    /// What it's doing now.
    pub fn status(&self) -> &Status {
        &self.status
    }

    /// Bring the hold in line with the setting (`wanted`) and the power, at
    /// `now`. Returns the status, which changes only here.
    pub fn tick(&mut self, wanted: bool, power: Power, now: Instant) -> &Status {
        if !wanted {
            self.heal();
            self.let_go();
            self.forget_failures();
            return self.set(Status::Off);
        }
        if let Err(pause) = verdict(power, self.holding) {
            self.heal();
            self.let_go();
            // A pause is no failure: once the power allows, the hold is
            // tried at once, and no wait from before it shows as a status.
            self.forget_failures();
            return self.set(Status::Paused(pause));
        }
        if self.holding {
            match self.backend.check() {
                Check::Standing => {
                    if std::mem::take(&mut self.unsure) > 0 {
                        log::info!("keep awake: the hold checks out again");
                    }
                }
                Check::Lost => {
                    log::warn!("keep awake: the hold was lost; taking it again");
                    // Whatever is left of it goes before the next attempt.
                    self.let_go();
                }
                Check::Unknown(e) => {
                    if self.unsure == 0 {
                        log::warn!("keep awake: couldn't check the hold ({e}); keeping it and checking again");
                    }
                    self.unsure = self.unsure.saturating_add(1);
                }
            }
        }
        if !self.holding && self.retry_at.is_none_or(|at| now >= at) {
            self.settled = true;
            match self.backend.hold() {
                Ok(()) => {
                    self.holding = true;
                    // The hold took over whatever was left to undo.
                    self.owed = false;
                    self.forget_failures();
                }
                Err(e) => {
                    // It may have left its own undoing unfinished.
                    self.owed = true;
                    let wait = RETRY_SECS[self.strikes.min(RETRY_SECS.len() - 1)];
                    self.strikes += 1;
                    self.retry_at = Some(now + Duration::from_secs(wait));
                    return self.set(Status::Failed(e));
                }
            }
        }
        if self.holding {
            // A hold nobody has been able to check for a while is still
            // kept, and no longer vouched for.
            let caveat = if self.unsure >= PATIENCE { Some(UNCONFIRMED.to_owned()) } else { self.backend.caveat() };
            self.set(caveat.map_or(Status::Holding, Status::Limited))
        } else {
            // Waiting out a retry. Only a failed hold sets the wait, and
            // off and paused clear it, so the status here is that failure.
            &self.status
        }
    }

    /// Let go for good (quit). The setting is untouched. A tick after it
    /// starts fresh, with no wait left from an earlier failure.
    pub fn stop(&mut self) {
        self.let_go();
        self.forget_failures();
        self.set(Status::Off);
    }

    fn forget_failures(&mut self) {
        self.strikes = 0;
        self.retry_at = None;
    }

    fn heal(&mut self) {
        if !std::mem::replace(&mut self.settled, true) {
            if let Err(e) = self.backend.heal() {
                log::error!("keep awake: couldn't put back what an earlier run left ({e}); trying again");
                self.complained = true;
                self.owed = true;
            }
        }
    }

    fn let_go(&mut self) {
        if !self.holding && !self.owed {
            return;
        }
        self.holding = false;
        self.unsure = 0;
        match self.backend.release() {
            Ok(()) => {
                if std::mem::take(&mut self.complained) {
                    log::info!("keep awake: the computer is back as it was");
                }
                self.owed = false;
            }
            Err(e) => {
                if !std::mem::replace(&mut self.complained, true) {
                    log::error!("keep awake: couldn't put the computer back as it was ({e}); trying again");
                }
                self.owed = true;
            }
        }
    }

    fn set(&mut self, status: Status) -> &Status {
        if self.status != status {
            match &status {
                Status::Off => log::info!("keep awake: off"),
                Status::Holding => log::info!("keep awake: holding (the computer stays awake with the lid closed)"),
                Status::Limited(what) => log::warn!("keep awake: on, with a caveat ({what})"),
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
///
/// A record also names the run that saved it ([`RestoreFile::owned_by`]),
/// so that run's guard process puts back only its own app's change and
/// never one a later run has made since.
#[derive(Debug, Clone)]
pub struct RestoreFile {
    path: PathBuf,
    owner: Option<String>,
}

/// The file's contents, as saved.
#[derive(Serialize)]
struct Saving<'a, T> {
    owner: Option<&'a str>,
    record: &'a T,
}

/// The file's contents, as loaded.
#[derive(Deserialize)]
struct Loaded<T> {
    #[serde(default)]
    owner: Option<String>,
    record: T,
}

impl RestoreFile {
    /// The record at `<state_dir>/keep-awake-restore.json`.
    pub fn in_dir(state_dir: &Path) -> Self {
        RestoreFile { path: state_dir.join("keep-awake-restore.json"), owner: None }
    }

    /// Records saved through this handle name `owner` as the run that made
    /// them.
    pub fn owned_by(mut self, owner: impl Into<String>) -> Self {
        self.owner = Some(owner.into());
        self
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
        let mut file = fs::File::create(&tmp)?;
        let saving = Saving { owner: self.owner.as_deref(), record: original };
        file.write_all(&serde_json::to_vec_pretty(&saving).map_err(io::Error::other)?)?;
        // On disk before the setting changes: the record has to outlive a
        // power cut right after.
        file.sync_all()?;
        drop(file);
        fs::rename(&tmp, &self.path)
    }

    /// The saved value: `Ok(None)` with no record (nothing to put back). A
    /// record that can't be read is an error, and stays; one that reads and
    /// doesn't parse is [`io::ErrorKind::InvalidData`].
    pub fn load<T: for<'de> Deserialize<'de>>(&self) -> io::Result<Option<T>> {
        Ok(self.load_owned()?.map(|(_, record)| record))
    }

    /// [`load`](RestoreFile::load) with the run that saved the record
    /// (`None`: it names nobody), both from one read of the file: whoever
    /// acts on the record only if it is a certain run's acts on the very
    /// record it checked.
    pub fn load_owned<T: for<'de> Deserialize<'de>>(&self) -> io::Result<Option<(Option<String>, T)>> {
        match fs::read(&self.path) {
            Ok(bytes) => serde_json::from_slice::<Loaded<T>>(&bytes)
                .map(|loaded| Some((loaded.owner, loaded.record)))
                .map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e)),
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
        /// What the next check says (then back to standing).
        check: Rc<RefCell<Option<Check>>>,
        /// Releases fail.
        stuck: Rc<RefCell<bool>>,
        /// The heal fails.
        sick: bool,
        caveat: Option<&'static str>,
    }

    impl Backend for Fake {
        fn heal(&mut self) -> Result<(), String> {
            if self.sick {
                self.log.borrow_mut().push("heal-failed");
                return Err("sick".into());
            }
            self.log.borrow_mut().push("heal");
            Ok(())
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
        fn check(&mut self) -> Check {
            self.check.borrow_mut().take().unwrap_or(Check::Standing)
        }
        fn release(&mut self) -> Result<(), String> {
            if *self.stuck.borrow() {
                self.log.borrow_mut().push("release-failed");
                return Err("stuck".into());
            }
            self.log.borrow_mut().push("release");
            Ok(())
        }
    }

    type Shared<T> = Rc<RefCell<T>>;
    type Calls = Shared<Vec<&'static str>>;

    fn keeper() -> (Keeper<Fake>, Calls, Shared<bool>, Shared<Option<Check>>) {
        let fake = Fake::default();
        let (log, fail, check) = (fake.log.clone(), fake.fail.clone(), fake.check.clone());
        (Keeper::new(fake), log, fail, check)
    }

    /// `secs` on the test's own monotonic clock.
    fn at(secs: u64) -> Instant {
        static START: std::sync::OnceLock<Instant> = std::sync::OnceLock::new();
        *START.get_or_init(Instant::now) + Duration::from_secs(secs)
    }

    const AC: Power = Power { external: Some(true), battery_pct: Some(50) };
    fn battery(pct: u8) -> Power {
        Power { external: Some(false), battery_pct: Some(pct) }
    }

    #[test]
    fn holds_while_on_and_releases_when_off() {
        let (mut k, log, ..) = keeper();
        assert!(log.borrow().is_empty(), "nothing is touched before the first tick");
        assert_eq!(k.tick(false, AC, at(0)), &Status::Off);
        assert_eq!(*log.borrow(), ["heal"], "an earlier run's leftovers go first");
        assert_eq!(k.tick(true, AC, at(1)), &Status::Holding);
        assert_eq!(k.tick(true, AC, at(2)), &Status::Holding);
        assert_eq!(*log.borrow(), ["heal", "hold"], "held once, not once per tick");
        assert_eq!(k.tick(false, AC, at(3)), &Status::Off);
        assert_eq!(*log.borrow(), ["heal", "hold", "release"]);
        assert_eq!(k.tick(false, AC, at(4)), &Status::Off);
        assert_eq!(log.borrow().len(), 3, "nothing to release twice");
    }

    #[test]
    fn quitting_and_dropping_release() {
        let (mut k, log, ..) = keeper();
        k.tick(true, AC, at(0));
        k.stop();
        assert_eq!(*log.borrow(), ["hold", "release"]);
        assert_eq!(k.status(), &Status::Off);
        drop(k);
        assert_eq!(log.borrow().len(), 2, "already released");

        let (mut k, log, ..) = keeper();
        k.tick(true, AC, at(0));
        drop(k);
        assert_eq!(*log.borrow(), ["hold", "release"], "a keeper dropped while holding lets go");
    }

    #[test]
    fn the_battery_floor_pauses_and_the_margin_resumes() {
        let (mut k, log, ..) = keeper();
        assert_eq!(k.tick(true, battery(60), at(0)), &Status::Holding, "on battery above the floor");
        assert_eq!(k.tick(true, battery(BATTERY_FLOOR + 1), at(1)), &Status::Holding);
        assert_eq!(k.tick(true, battery(BATTERY_FLOOR), at(2)), &Status::Paused(Pause::BatteryLow(BATTERY_FLOOR)));
        assert_eq!(*log.borrow(), ["hold", "release"], "at the floor the computer may sleep");
        let nearly = BATTERY_FLOOR + BATTERY_RESUME - 1;
        assert_eq!(k.tick(true, battery(nearly), at(3)), &Status::Paused(Pause::BatteryLow(nearly)), "no flapping");
        assert_eq!(k.tick(true, battery(BATTERY_FLOOR + BATTERY_RESUME), at(4)), &Status::Holding);
        assert_eq!(k.tick(true, battery(5), at(5)), &Status::Paused(Pause::BatteryLow(5)));
        assert_eq!(
            k.tick(true, Power { external: Some(true), battery_pct: Some(5) }, at(6)),
            &Status::Holding,
            "plugged in"
        );
        assert_eq!(*log.borrow(), ["hold", "release", "hold", "release", "hold"]);
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
        assert_eq!(k.tick(true, AC, at(100)), &Status::Failed("refused".into()));
        assert_eq!(k.tick(true, AC, at(101)), &Status::Failed("refused".into()), "still failed while waiting");
        assert_eq!(*log.borrow(), ["hold-failed"], "not hammered every tick");
        k.tick(true, AC, at(100 + RETRY_SECS[0]));
        assert_eq!(log.borrow().len(), 2, "the first retry");
        k.tick(true, AC, at(100 + RETRY_SECS[0] + RETRY_SECS[1] - 1));
        assert_eq!(log.borrow().len(), 2, "the waits grow");
        *fail.borrow_mut() = false;
        assert_eq!(k.tick(true, AC, at(100 + RETRY_SECS[0] + RETRY_SECS[1])), &Status::Holding);
        // Switching off and on again starts fresh, with no wait.
        *fail.borrow_mut() = true;
        k.tick(false, AC, at(1000));
        k.tick(true, AC, at(1001));
        let before = log.borrow().len();
        k.tick(false, AC, at(1002));
        assert_eq!(log.borrow()[before..], ["release"], "a hold that failed may have left its undoing unfinished");
        k.tick(false, AC, at(1003));
        assert_eq!(log.borrow().len(), before + 1, "undone once");
        *fail.borrow_mut() = false;
        assert_eq!(k.tick(true, AC, at(1004)), &Status::Holding, "a new tick of the box tries at once");
    }

    #[test]
    fn a_failed_heal_is_followed_by_releases_until_one_goes_through() {
        let fake = Fake { sick: true, ..Fake::default() };
        let (log, stuck) = (fake.log.clone(), fake.stuck.clone());
        let mut k = Keeper::new(fake);
        *stuck.borrow_mut() = true;
        k.tick(false, AC, at(0));
        k.tick(false, AC, at(1));
        *stuck.borrow_mut() = false;
        k.tick(false, AC, at(2));
        k.tick(false, AC, at(3));
        assert_eq!(*log.borrow(), ["heal-failed", "release-failed", "release-failed", "release"]);
    }

    #[test]
    fn a_failed_release_is_tried_again_until_it_goes_through() {
        let fake = Fake::default();
        let (log, stuck) = (fake.log.clone(), fake.stuck.clone());
        let mut k = Keeper::new(fake);
        k.tick(true, AC, at(0));
        *stuck.borrow_mut() = true;
        assert_eq!(k.tick(false, AC, at(1)), &Status::Off);
        assert_eq!(k.tick(false, AC, at(2)), &Status::Off);
        assert_eq!(*log.borrow(), ["hold", "release-failed", "release-failed"], "the OS is still changed: again");
        *stuck.borrow_mut() = false;
        k.tick(false, AC, at(3));
        k.tick(false, AC, at(4));
        assert_eq!(log.borrow()[3..], ["release"], "and no more once it is back");

        // Ticking the box again takes the leftovers over instead.
        *stuck.borrow_mut() = true;
        k.tick(true, AC, at(5));
        k.tick(false, AC, at(6));
        k.tick(true, AC, at(7));
        *stuck.borrow_mut() = false;
        k.tick(false, AC, at(8));
        k.tick(false, AC, at(9));
        assert_eq!(log.borrow()[4..], ["hold", "release-failed", "hold", "release"]);

        // Quit tries too.
        let fake = Fake::default();
        let (log, stuck) = (fake.log.clone(), fake.stuck.clone());
        let mut k = Keeper::new(fake);
        k.tick(true, AC, at(0));
        *stuck.borrow_mut() = true;
        k.tick(false, AC, at(1));
        *stuck.borrow_mut() = false;
        k.stop();
        assert_eq!(*log.borrow(), ["hold", "release-failed", "release"]);
    }

    #[test]
    fn a_lost_hold_is_taken_again() {
        let (mut k, log, _, check) = keeper();
        k.tick(true, AC, at(0));
        *check.borrow_mut() = Some(Check::Lost);
        assert_eq!(k.tick(true, AC, at(1)), &Status::Holding);
        assert_eq!(*log.borrow(), ["hold", "release", "hold"], "what was left is released first");
    }

    #[test]
    fn a_check_that_fails_keeps_the_hold() {
        // Releasing here would end the hold the check was meant to keep:
        // on macOS that clears the lid flag, and a shut Mac sleeps at once.
        let (mut k, log, fail, check) = keeper();
        k.tick(true, AC, at(0));
        // Even a hold that couldn't be taken again right now is kept.
        *fail.borrow_mut() = true;
        for t in 1..4 {
            *check.borrow_mut() = Some(Check::Unknown("no answer".into()));
            assert_eq!(k.tick(true, AC, at(t)), &Status::Holding, "tick {t}");
        }
        assert_eq!(*log.borrow(), ["hold"], "neither released nor taken again");
        // A check that keeps failing is still no reason to let go, but the
        // menu stops vouching for the hold.
        *check.borrow_mut() = Some(Check::Unknown("no answer".into()));
        assert_eq!(k.tick(true, AC, at(4)), &Status::Limited(UNCONFIRMED.into()), "{PATIENCE} in a row");
        assert_eq!(*log.borrow(), ["hold"]);
        assert_eq!(k.tick(true, AC, at(4)), &Status::Holding, "and the next check that works finds it standing");
        // Unticking after a failed check still lets go.
        *check.borrow_mut() = Some(Check::Unknown("no answer".into()));
        k.tick(true, AC, at(5));
        assert_eq!(k.tick(false, AC, at(6)), &Status::Off);
        assert_eq!(*log.borrow(), ["hold", "release"]);
    }

    #[test]
    fn a_failed_power_reading_stands_on_the_last_good_one_for_a_while() {
        let mut readings = Readings::default();
        assert_eq!(readings.settle(None), Power::UNREAD, "no reading yet: nothing to hold on");
        assert_eq!(readings.settle(Some(AC)), AC);
        // The reading fails (macOS: `pmset` didn't run). That says nothing
        // about the battery, so the hold isn't let go over it.
        for miss in 1..PATIENCE {
            assert_eq!(readings.settle(None), AC, "miss {miss}");
        }
        assert_eq!(readings.settle(None), Power::UNREAD, "a battery nobody can read for a minute isn't held on");
        assert_eq!(readings.settle(None), Power::UNREAD);
        assert_eq!(readings.settle(Some(battery(60))), battery(60));
        assert_eq!(readings.settle(None), battery(60), "the count starts over with a good reading");

        // Through the keeper: a plugged-in laptop keeps its hold over one miss.
        let (mut k, log, ..) = keeper();
        let mut readings = Readings::default();
        k.tick(true, readings.settle(Some(AC)), at(0));
        assert_eq!(k.tick(true, readings.settle(None), at(1)), &Status::Holding);
        assert_eq!(*log.borrow(), ["hold"]);
    }

    #[test]
    fn a_stop_clears_the_wait_too() {
        let (mut k, log, fail, _) = keeper();
        *fail.borrow_mut() = true;
        k.tick(true, AC, at(0));
        k.stop();
        *fail.borrow_mut() = false;
        assert_eq!(k.tick(true, AC, at(1)), &Status::Holding, "not left waiting out the old failure as Off");
        assert_eq!(*log.borrow(), ["hold-failed", "release", "hold"]);
    }

    #[test]
    fn a_pause_clears_the_wait_of_an_earlier_failure() {
        let (mut k, log, fail, _) = keeper();
        *fail.borrow_mut() = true;
        // Three failures in a row: the next try is minutes away.
        k.tick(true, AC, at(0));
        k.tick(true, AC, at(RETRY_SECS[0]));
        k.tick(true, AC, at(RETRY_SECS[0] + RETRY_SECS[1]));
        assert_eq!(log.borrow().len(), 3);
        // The battery reaches the floor, then the charger goes in.
        let low = RETRY_SECS[0] + RETRY_SECS[1] + 1;
        assert_eq!(k.tick(true, battery(BATTERY_FLOOR), at(low)), &Status::Paused(Pause::BatteryLow(BATTERY_FLOOR)));
        assert_eq!(
            k.tick(true, AC, at(low + 1)),
            &Status::Failed("refused".into()),
            "plugged in: tried at once, and never shown as paused"
        );
        assert_eq!(log.borrow()[3..], ["release", "hold-failed"], "the failed holds' undoing, then the new try");
        *fail.borrow_mut() = false;
        assert_eq!(k.tick(true, AC, at(low + 1 + RETRY_SECS[0])), &Status::Holding, "and the waits start over");
    }

    #[test]
    fn leftovers_are_healed_once_unless_the_first_tick_holds() {
        // A start that holds takes the leftovers over: healing first would
        // let go of a lid that is shut.
        let (mut k, log, ..) = keeper();
        k.tick(true, AC, at(0));
        k.tick(false, AC, at(1));
        assert_eq!(*log.borrow(), ["hold", "release"], "the hold dealt with them");

        // A start that is paused holds nothing, so it heals.
        let (mut k, log, ..) = keeper();
        k.tick(true, battery(3), at(0));
        k.tick(true, battery(3), at(1));
        k.tick(false, AC, at(2));
        assert_eq!(*log.borrow(), ["heal"], "once");
    }

    #[test]
    fn a_desktop_that_keeps_the_lid_shows_as_limited() {
        let mut k = Keeper::new(Fake { caveat: Some("set the lid by hand"), ..Fake::default() });
        assert_eq!(k.tick(true, AC, at(0)), &Status::Limited("set the lid by hand".into()));
        assert_eq!(k.status().note().as_deref(), Some("set the lid by hand"));
        assert_eq!(k.tick(false, AC, at(1)), &Status::Off);
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
        assert_eq!(
            file.load::<Vec<u32>>().unwrap_err().kind(),
            io::ErrorKind::InvalidData,
            "a record that doesn't parse is not 'nothing to put back'"
        );
        assert!(file.path().exists());
        file.clear().unwrap();
        assert_eq!(file.load::<Vec<u32>>().unwrap(), None);
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn a_record_names_the_run_that_saved_it() {
        let dir = std::env::temp_dir().join(format!("cm-keep-awake-owner-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        let anyone = RestoreFile::in_dir(&dir);
        assert_eq!(anyone.load_owned::<u32>().unwrap(), None, "no record");
        let first = RestoreFile::in_dir(&dir).owned_by("run-1");
        first.save(&7u32).unwrap();
        assert_eq!(anyone.load_owned::<u32>().unwrap(), Some((Some("run-1".into()), 7)));
        assert_eq!(anyone.load::<u32>().unwrap(), Some(7), "any handle reads it");
        // A later run takes over: the first run's guard must see it isn't its own.
        RestoreFile::in_dir(&dir).owned_by("run-2").save(&8u32).unwrap();
        assert_eq!(
            first.load_owned::<u32>().unwrap(),
            Some((Some("run-2".into()), 8)),
            "the owner and its record together"
        );
        anyone.save(&9u32).unwrap();
        assert_eq!(anyone.load_owned::<u32>().unwrap(), Some((None, 9)), "saved by nobody in particular");
        fs::write(anyone.path(), b"{").unwrap();
        assert!(anyone.load_owned::<u32>().is_err());
        let _ = fs::remove_dir_all(&dir);
    }
}
