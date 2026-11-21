//! Windows: keep awake with the lid closed by editing the power plan.
//!
//! No Windows power request stops the sleep a closed lid starts, so the only
//! way is the plan's own "When I close the lid" setting. While holding,
//! [`PlanHold`] sets, in the active plan:
//!
//! - the lid action to "Do nothing", plugged in and on battery;
//! - "Sleep after" on battery to "Never": on a Modern Standby laptop Windows
//!   ends a power request five minutes after that timeout, and the app's
//!   request is what holds off idle sleep.
//!
//! These settings outlive the process, so the old values go into the
//! [`RestoreFile`] before anything is written, and come back on release, on
//! quit, at the end of the Windows session and on the next start after a
//! crash. Only a value that still reads as this module wrote it is put
//! back: one the user changed in the meantime is theirs.
//!
//! The Win32 calls are the app crate's [`Plan`]; this half is the logic,
//! which the tests run over a fake.

use serde::{Deserialize, Serialize};

use super::{Power, RestoreFile};

/// One of the plan's settings this module changes.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub enum Setting {
    /// "When I close the lid": 0 nothing, 1 sleep, 2 hibernate, 3 shut down.
    LidAction,
    /// "Sleep after", in seconds; 0 is never.
    SleepAfter,
}

/// Which of a setting's two values: plugged in or on battery.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub enum Source {
    /// Plugged in.
    Ac,
    /// On battery.
    Dc,
}

/// "Do nothing" as a lid action and "Never" as a sleep timeout.
pub const OFF: u32 = 0;

/// What a hold sets to [`OFF`].
const EDITS: [(Setting, Source); 3] =
    [(Setting::LidAction, Source::Ac), (Setting::LidAction, Source::Dc), (Setting::SleepAfter, Source::Dc)];

/// The power plans, as Windows has them. A plan is named by its GUID's text.
pub trait Plan {
    /// The plan in use.
    fn active(&mut self) -> Result<String, String>;
    /// A setting's value in `scheme`.
    fn read(&mut self, scheme: &str, setting: Setting, source: Source) -> Result<u32, String>;
    /// Store a setting's value in `scheme`. It takes effect on [`apply`].
    ///
    /// [`apply`]: Plan::apply
    fn write(&mut self, scheme: &str, setting: Setting, source: Source, value: u32) -> Result<(), String>;
    /// Make `scheme`, the plan in use, act on its stored values.
    fn apply(&mut self, scheme: &str) -> Result<(), String>;
    /// Whether a group policy sets this value, so the plan's has no say.
    fn locked(&mut self, _setting: Setting, _source: Source) -> bool {
        false
    }
}

/// One value a hold changed, and what it was.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
struct Change {
    setting: Setting,
    source: Source,
    original: u32,
}

/// The restore record: the plan a hold edited and the values to put back.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
struct Saved {
    scheme: String,
    changes: Vec<Change>,
}

/// Holds the lid open, so to speak, through a [`Plan`].
pub struct PlanHold<P: Plan> {
    plan: P,
    file: RestoreFile,
    /// The plan the hold was taken in.
    scheme: Option<String>,
}

impl<P: Plan> PlanHold<P> {
    /// A hold over `plan`, with its record in `file`. Touches nothing.
    pub fn new(plan: P, file: RestoreFile) -> Self {
        PlanHold { plan, file, scheme: None }
    }

    /// Edit the plan in use. An earlier record is put back first, so the
    /// values saved are never this module's own. `Err` leaves the plan as it
    /// was.
    pub fn hold(&mut self) -> Result<(), String> {
        self.restore()?;
        let scheme = self.plan.active()?;
        let mut changes = Vec::new();
        for (setting, source) in EDITS {
            if self.plan.locked(setting, source) {
                if setting == Setting::LidAction {
                    return Err("a group policy sets the lid action".into());
                }
                // The sleep timeout is the lesser half: hold without it.
                continue;
            }
            let original = self.plan.read(&scheme, setting, source)?;
            if original != OFF {
                changes.push(Change { setting, source, original });
            }
        }
        if !changes.is_empty() {
            let saved = Saved { scheme: scheme.clone(), changes };
            // The record first: a crash from here on is healed by the next
            // start.
            self.file.save(&saved).map_err(|e| format!("can't save {}: {e}", self.file.path().display()))?;
            if let Err(e) = self.write_all(&saved) {
                if let Err(undo) = self.restore() {
                    log::error!("keep awake: couldn't put the power plan back ({undo}); trying again later");
                }
                return Err(e);
            }
        }
        self.scheme = Some(scheme);
        Ok(())
    }

    fn write_all(&mut self, saved: &Saved) -> Result<(), String> {
        for c in &saved.changes {
            self.plan.write(&saved.scheme, c.setting, c.source, OFF)?;
        }
        self.plan.apply(&saved.scheme)?;
        for c in &saved.changes {
            if self.plan.read(&saved.scheme, c.setting, c.source)? != OFF {
                return Err("the power plan didn't take the change".into());
            }
        }
        Ok(())
    }

    /// Whether the hold still stands: the plan it edited is the one in use.
    /// A switch to another plan loses it, and the next [`hold`] moves it.
    ///
    /// [`hold`]: PlanHold::hold
    pub fn holding(&mut self) -> bool {
        match (&self.scheme, self.plan.active()) {
            (Some(held), Ok(active)) => *held == active,
            _ => false,
        }
    }

    /// Put back what the record says and drop it. With no record there is
    /// nothing to do. `Err` keeps the record for another try.
    pub fn restore(&mut self) -> Result<(), String> {
        self.scheme = None;
        let saved: Saved = match self.file.load() {
            Ok(None) => return Ok(()),
            Ok(Some(saved)) => saved,
            Err(e) => return Err(format!("can't read {}: {e}", self.file.path().display())),
        };
        let mut failed = None;
        for c in &saved.changes {
            match self.plan.read(&saved.scheme, c.setting, c.source) {
                Ok(OFF) => {
                    if let Err(e) = self.plan.write(&saved.scheme, c.setting, c.source, c.original) {
                        failed = Some(e);
                    }
                }
                // Changed since: the user's own choice stays.
                Ok(_) => {}
                // A plan that's gone has nothing to put back.
                Err(e) => log::warn!("keep awake: can't read {:?} ({:?}) to restore it: {e}", c.setting, c.source),
            }
        }
        // Only the plan in use is re-applied: applying another would switch
        // the user's plan.
        if self.plan.active().is_ok_and(|active| active == saved.scheme) {
            if let Err(e) = self.plan.apply(&saved.scheme) {
                failed = Some(e);
            }
        }
        if let Some(e) = failed {
            return Err(e);
        }
        self.file.clear().map_err(|e| format!("can't remove {}: {e}", self.file.path().display()))
    }
}

/// The computer's power from `GetSystemPowerStatus`: `ACLineStatus`,
/// `BatteryFlag` and `BatteryLifePercent`.
pub fn power(ac_line: u8, battery_flag: u8, battery_pct: u8) -> Power {
    /// `BatteryFlag`: no system battery.
    const NO_BATTERY: u8 = 128;
    /// Any of the three: unknown.
    const UNKNOWN: u8 = 255;
    let external = match ac_line {
        0 => Some(false),
        1 => Some(true),
        _ => None,
    };
    let has_battery = battery_flag != UNKNOWN && battery_flag & NO_BATTERY == 0;
    let battery_pct = (has_battery && battery_pct != UNKNOWN).then(|| battery_pct.min(100));
    Power { external, battery_pct }
}

/// A GUID's fields as the registry spells it: `381b4222-f694-41f0-9685-ff5bb260df2e`.
pub fn guid_text(data1: u32, data2: u16, data3: u16, data4: [u8; 8]) -> String {
    let d = data4;
    format!(
        "{data1:08x}-{data2:04x}-{data3:04x}-{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
        d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]
    )
}

/// [`guid_text`] back into fields; `None` when it isn't a GUID.
pub fn guid_fields(text: &str) -> Option<(u32, u16, u16, [u8; 8])> {
    let parts: Vec<&str> = text.split('-').collect();
    let lens = [8, 4, 4, 4, 12];
    if parts.len() != lens.len()
        || parts.iter().zip(lens).any(|(p, len)| p.len() != len || !p.bytes().all(|b| b.is_ascii_hexdigit()))
    {
        return None;
    }
    let tail = format!("{}{}", parts[3], parts[4]);
    let mut data4 = [0u8; 8];
    for (i, byte) in data4.iter_mut().enumerate() {
        *byte = u8::from_str_radix(&tail[i * 2..i * 2 + 2], 16).ok()?;
    }
    Some((
        u32::from_str_radix(parts[0], 16).ok()?,
        u16::from_str_radix(parts[1], 16).ok()?,
        u16::from_str_radix(parts[2], 16).ok()?,
        data4,
    ))
}

#[cfg(test)]
mod tests {
    use std::cell::RefCell;
    use std::collections::HashMap;
    use std::rc::Rc;

    use super::*;

    const BALANCED: &str = "381b4222-f694-41f0-9685-ff5bb260df2e";
    const SAVER: &str = "a1841308-3541-4fab-bc81-f71556f20b4a";

    type Key = (String, Setting, Source);

    /// The plans in memory: nothing of Windows is touched.
    #[derive(Default)]
    struct State {
        active: String,
        values: HashMap<Key, u32>,
        /// Plans made active by `apply`, in order.
        applied: Vec<String>,
        locked: Vec<(Setting, Source)>,
        fail_write: bool,
        fail_apply: bool,
        /// Writes are accepted and not kept.
        ignore_writes: bool,
    }

    #[derive(Clone, Default)]
    struct Fake(Rc<RefCell<State>>);

    impl Plan for Fake {
        fn active(&mut self) -> Result<String, String> {
            Ok(self.0.borrow().active.clone())
        }
        fn read(&mut self, scheme: &str, setting: Setting, source: Source) -> Result<u32, String> {
            self.0.borrow().values.get(&(scheme.to_owned(), setting, source)).copied().ok_or("no such plan".into())
        }
        fn write(&mut self, scheme: &str, setting: Setting, source: Source, value: u32) -> Result<(), String> {
            let mut s = self.0.borrow_mut();
            if s.fail_write {
                return Err("access denied".into());
            }
            if !s.ignore_writes {
                s.values.insert((scheme.to_owned(), setting, source), value);
            }
            Ok(())
        }
        fn apply(&mut self, scheme: &str) -> Result<(), String> {
            let mut s = self.0.borrow_mut();
            if s.fail_apply {
                return Err("can't apply".into());
            }
            s.applied.push(scheme.to_owned());
            s.active = scheme.to_owned();
            Ok(())
        }
        fn locked(&mut self, setting: Setting, source: Source) -> bool {
            self.0.borrow().locked.contains(&(setting, source))
        }
    }

    /// A laptop on Balanced: the lid sleeps it, and it sleeps after 15
    /// minutes on battery. The Power saver plan is there too.
    fn laptop() -> Fake {
        let fake = Fake::default();
        {
            let mut s = fake.0.borrow_mut();
            s.active = BALANCED.into();
            for scheme in [BALANCED, SAVER] {
                s.values.insert((scheme.into(), Setting::LidAction, Source::Ac), 1);
                s.values.insert((scheme.into(), Setting::LidAction, Source::Dc), 2);
                s.values.insert((scheme.into(), Setting::SleepAfter, Source::Ac), 1800);
                s.values.insert((scheme.into(), Setting::SleepAfter, Source::Dc), 900);
            }
        }
        fake
    }

    fn value(fake: &Fake, scheme: &str, setting: Setting, source: Source) -> u32 {
        fake.0.borrow().values[&(scheme.to_owned(), setting, source)]
    }

    /// The four values of one plan: lid AC, lid DC, sleep AC, sleep DC.
    fn plan_of(fake: &Fake, scheme: &str) -> [u32; 4] {
        [
            value(fake, scheme, Setting::LidAction, Source::Ac),
            value(fake, scheme, Setting::LidAction, Source::Dc),
            value(fake, scheme, Setting::SleepAfter, Source::Ac),
            value(fake, scheme, Setting::SleepAfter, Source::Dc),
        ]
    }

    fn scratch(name: &str) -> RestoreFile {
        let dir = std::env::temp_dir().join(format!("cm-keep-awake-plan-{name}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        RestoreFile::in_dir(&dir)
    }

    #[test]
    fn a_hold_edits_the_plan_and_a_restore_puts_it_back() {
        let (fake, file) = (laptop(), scratch("roundtrip"));
        let mut hold = PlanHold::new(fake.clone(), file.clone());
        assert!(!hold.holding());
        hold.hold().unwrap();
        assert_eq!(plan_of(&fake, BALANCED), [0, 0, 1800, 0], "lid: nothing; battery sleep: never; AC sleep untouched");
        assert_eq!(plan_of(&fake, SAVER), [1, 2, 1800, 900], "only the plan in use");
        assert_eq!(fake.0.borrow().applied, [BALANCED], "applied, so it takes effect");
        assert!(file.path().exists(), "the old values are on disk");
        assert!(hold.holding());

        hold.restore().unwrap();
        assert_eq!(plan_of(&fake, BALANCED), [1, 2, 1800, 900]);
        assert_eq!(fake.0.borrow().applied, [BALANCED, BALANCED]);
        assert!(!file.path().exists(), "nothing left to put back");
        assert!(!hold.holding());
        hold.restore().unwrap();
        assert_eq!(fake.0.borrow().applied.len(), 2, "a second restore has nothing to do");
    }

    #[test]
    fn the_next_start_heals_a_crash() {
        let (fake, file) = (laptop(), scratch("crash"));
        PlanHold::new(fake.clone(), file.clone()).hold().unwrap();
        // The process dies here: no restore. A new one starts.
        let mut next = PlanHold::new(fake.clone(), file.clone());
        assert_eq!(plan_of(&fake, BALANCED), [0, 0, 1800, 0]);
        next.restore().unwrap();
        assert_eq!(plan_of(&fake, BALANCED), [1, 2, 1800, 900]);
        assert!(!file.path().exists());
    }

    #[test]
    fn a_hold_over_a_leftover_record_keeps_the_real_originals() {
        let (fake, file) = (laptop(), scratch("leftover"));
        PlanHold::new(fake.clone(), file.clone()).hold().unwrap();
        // A crash, then a start that holds at once.
        let mut next = PlanHold::new(fake.clone(), file.clone());
        next.hold().unwrap();
        assert_eq!(plan_of(&fake, BALANCED), [0, 0, 1800, 0]);
        next.restore().unwrap();
        assert_eq!(plan_of(&fake, BALANCED), [1, 2, 1800, 900], "not the zeros the first hold wrote");
    }

    #[test]
    fn a_value_the_user_changed_is_left_alone() {
        let (fake, file) = (laptop(), scratch("user"));
        let mut hold = PlanHold::new(fake.clone(), file);
        hold.hold().unwrap();
        // The user sets the battery lid action to "Shut down" meanwhile.
        fake.0.borrow_mut().values.insert((BALANCED.into(), Setting::LidAction, Source::Dc), 3);
        hold.restore().unwrap();
        assert_eq!(plan_of(&fake, BALANCED), [1, 3, 1800, 900]);
    }

    #[test]
    fn values_already_off_are_neither_recorded_nor_changed_back() {
        let (fake, file) = (laptop(), scratch("already"));
        for source in [Source::Ac, Source::Dc] {
            fake.0.borrow_mut().values.insert((BALANCED.into(), Setting::LidAction, source), OFF);
        }
        let mut hold = PlanHold::new(fake.clone(), file.clone());
        hold.hold().unwrap();
        hold.restore().unwrap();
        assert_eq!(plan_of(&fake, BALANCED), [0, 0, 1800, 900], "the user's own Do nothing stays");

        // Nothing to change at all: no record, no apply, and still a hold.
        fake.0.borrow_mut().values.insert((BALANCED.into(), Setting::SleepAfter, Source::Dc), OFF);
        let applied = fake.0.borrow().applied.len();
        hold.hold().unwrap();
        assert!(hold.holding());
        assert!(!file.path().exists());
        assert_eq!(fake.0.borrow().applied.len(), applied);
    }

    #[test]
    fn switching_plans_moves_the_hold() {
        let (fake, file) = (laptop(), scratch("switch"));
        let mut hold = PlanHold::new(fake.clone(), file);
        hold.hold().unwrap();
        fake.0.borrow_mut().active = SAVER.into();
        assert!(!hold.holding(), "the plan in use isn't the one edited");
        let applied = fake.0.borrow().applied.len();
        hold.hold().unwrap();
        assert_eq!(plan_of(&fake, BALANCED), [1, 2, 1800, 900], "the old plan is as it was");
        assert_eq!(plan_of(&fake, SAVER), [0, 0, 1800, 0]);
        assert_eq!(
            fake.0.borrow().applied[applied..],
            [SAVER],
            "the old plan is never applied: that would switch back"
        );
        assert!(hold.holding());
        hold.restore().unwrap();
        assert_eq!(plan_of(&fake, SAVER), [1, 2, 1800, 900]);
    }

    #[test]
    fn a_refused_write_leaves_the_plan_as_it_was() {
        let (fake, file) = (laptop(), scratch("refused"));
        let mut hold = PlanHold::new(fake.clone(), file.clone());
        fake.0.borrow_mut().fail_write = true;
        assert_eq!(hold.hold(), Err("access denied".into()));
        assert_eq!(plan_of(&fake, BALANCED), [1, 2, 1800, 900]);
        assert!(!file.path().exists());
        assert!(!hold.holding());

        // Applied but not kept (a policy the check didn't see): undone too.
        let mut s = fake.0.borrow_mut();
        s.fail_write = false;
        s.ignore_writes = true;
        drop(s);
        assert_eq!(hold.hold(), Err("the power plan didn't take the change".into()));
        assert!(!file.path().exists());
    }

    #[test]
    fn a_failed_apply_is_undone_and_a_failed_restore_keeps_the_record() {
        let (fake, file) = (laptop(), scratch("apply"));
        let mut hold = PlanHold::new(fake.clone(), file.clone());
        fake.0.borrow_mut().fail_apply = true;
        assert_eq!(hold.hold(), Err("can't apply".into()));
        assert_eq!(plan_of(&fake, BALANCED), [1, 2, 1800, 900], "the values are back");
        assert!(file.path().exists(), "but the apply failed, so the record waits for another try");
        fake.0.borrow_mut().fail_apply = false;
        hold.hold().unwrap();
        hold.restore().unwrap();
        assert_eq!(plan_of(&fake, BALANCED), [1, 2, 1800, 900]);
        assert!(!file.path().exists());
    }

    #[test]
    fn group_policy_on_the_lid_refuses_and_on_the_timeout_is_skipped() {
        let (fake, file) = (laptop(), scratch("policy"));
        let mut hold = PlanHold::new(fake.clone(), file);
        fake.0.borrow_mut().locked = vec![(Setting::SleepAfter, Source::Dc)];
        hold.hold().unwrap();
        assert_eq!(plan_of(&fake, BALANCED), [0, 0, 1800, 900], "held without the timeout");
        hold.restore().unwrap();
        fake.0.borrow_mut().locked = vec![(Setting::LidAction, Source::Dc)];
        assert_eq!(hold.hold(), Err("a group policy sets the lid action".into()));
        assert_eq!(plan_of(&fake, BALANCED), [1, 2, 1800, 900]);
    }

    #[test]
    fn a_plan_that_is_gone_doesnt_block_the_restore() {
        let (fake, file) = (laptop(), scratch("gone"));
        let mut hold = PlanHold::new(fake.clone(), file.clone());
        hold.hold().unwrap();
        let mut s = fake.0.borrow_mut();
        s.values.retain(|(scheme, ..), _| scheme != BALANCED);
        s.active = SAVER.into();
        drop(s);
        hold.restore().unwrap();
        assert!(!file.path().exists());
    }

    #[test]
    fn an_unreadable_record_is_an_error_not_a_clean_slate() {
        let (fake, file) = (laptop(), scratch("unreadable"));
        std::fs::create_dir_all(file.path().parent().unwrap()).unwrap();
        std::fs::write(file.path(), b"{").unwrap();
        let mut hold = PlanHold::new(fake.clone(), file.clone());
        assert!(hold.hold().is_err(), "holding now would save this module's own zeros as the originals");
        assert_eq!(plan_of(&fake, BALANCED), [1, 2, 1800, 900]);
    }

    #[test]
    fn power_from_the_system_status() {
        assert_eq!(power(1, 8, 100), Power { external: Some(true), battery_pct: Some(100) }, "charging");
        assert_eq!(power(0, 1, 64), Power { external: Some(false), battery_pct: Some(64) });
        assert_eq!(power(1, 128, 255), Power { external: Some(true), battery_pct: None }, "a desktop");
        assert_eq!(power(255, 255, 255), Power::UNKNOWN);
        assert_eq!(power(0, 0, 255), Power { external: Some(false), battery_pct: None }, "on battery, charge unread");
        assert_eq!(power(0, 2, 101).battery_pct, Some(100));
    }

    #[test]
    fn guids_round_trip() {
        let fields = (0x381b4222, 0xf694, 0x41f0, [0x96, 0x85, 0xff, 0x5b, 0xb2, 0x60, 0xdf, 0x2e]);
        assert_eq!(guid_text(fields.0, fields.1, fields.2, fields.3), BALANCED);
        assert_eq!(guid_fields(BALANCED), Some(fields));
        assert_eq!(guid_fields(&BALANCED.to_uppercase()), Some(fields));
        for bad in [
            "",
            "381b4222",
            "381b4222-f694-41f0-9685-ff5bb260df2",
            "381b4222-f694-41f0-9685-ff5bb260df2g",
            "+81b4222-f694-41f0-9685-ff5bb260df2e",
        ] {
            assert_eq!(guid_fields(bad), None, "{bad:?}");
        }
    }
}
