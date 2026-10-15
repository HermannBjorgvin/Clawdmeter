//! The daemon's decisions, with no I/O: what to poll, when, and what to write
//! to the device. The runtime (app crate) feeds it events, poll results and
//! the clock, and performs the writes it asks for.

use std::collections::{BTreeMap, HashMap};
use std::path::PathBuf;
use std::time::SystemTime;

use chrono::{DateTime, TimeZone, Utc};

use crate::attention::{Alert, AttentionState};
use crate::backoff::SavedBlock;
use crate::cadence::{heartbeat_due, AccountClock, Cadence, PollReason, Scheduler};
use crate::config::Settings;
use crate::events::{Activity, HookEvent, Signal};
use crate::payload::{clock_fields, Payload};
use crate::probe::{self, Plan as ProbePlan, Source};
use crate::rate::{Mood, RateTracker};
use crate::selector::PlanSelector;
use crate::usage::{AccountState, UsageError, UsageReading};

/// `a` changes are sent at most this often (s).
pub const ACTIVITY_MIN_INTERVAL: i64 = 5;
/// `e`/`es` ride along for this long after the event (s), so a device that
/// reconnects or reboots later doesn't react to an old event.
pub const EVENT_FRESH_SECS: i64 = 60;

/// Why a write happens.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum WriteKind {
    /// Fresh `/usage` numbers.
    Poll,
    /// Attention alert or clear.
    Attention,
    /// A hook event / activity / session-count change.
    Event,
    /// Keep-alive replay.
    Heartbeat,
    /// Every account is logged out: `{"ok":false}`.
    NoData,
}

/// A write the runtime should perform.
#[derive(Debug, Clone, PartialEq)]
pub struct Write {
    /// What to send.
    pub payload: Payload,
    /// Why.
    pub kind: WriteKind,
}

/// Host facts the payload needs at write time.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct HostClock {
    /// Local UTC offset (s), for `t`.
    pub utc_offset_secs: i64,
    /// `clock = auto` resolves to 12h.
    pub prefers_12h: bool,
}

/// A `/usage` poll to run now.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PollPlan {
    /// Why (the earliest-due account's reason).
    pub reason: PollReason,
    /// The accounts due now; the rest wait (cadence, gap or backoff).
    pub accounts: Vec<PathBuf>,
    /// How to read each of `accounts` (same order): the probe, `/usage`, or
    /// a reading known without a call ([`crate::probe::choose_source`]).
    pub plans: Vec<ProbePlan>,
}

/// One account's last poll error, for the settings window.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct AccountError {
    /// The account (config dir).
    pub dir: PathBuf,
    /// What to tell the user.
    pub message: String,
    /// The app is retrying on its own (rate limit, timeout, odd output);
    /// otherwise the user has to act.
    pub retrying: bool,
}

/// Snapshot for the tray / status output.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct EngineStatus {
    /// Account shown on the device.
    pub active_account: Option<PathBuf>,
    /// Its latest numbers.
    pub reading: Option<UsageReading>,
    /// Seconds since those numbers were read.
    pub age_secs: Option<i64>,
    /// Three misses in a row on the shown account.
    pub stale: bool,
    /// Every account failed with a login problem.
    pub logged_out: bool,
    /// Per-account last error, for the settings window.
    pub errors: Vec<AccountError>,
    /// An attention alert is up.
    pub attention: bool,
    /// Hook activity level 0-3.
    pub activity: u8,
    /// Sessions active in the last 5 min.
    pub sessions: u32,
    /// Hooks have been seen (fast clock available).
    pub hooks: bool,
    /// Mood from the rate math.
    pub mood: Option<Mood>,
    /// Unix seconds of the next scheduled poll.
    pub next_poll_at: Option<i64>,
    /// The earliest a refresh of the shown account can poll it (the
    /// per-account gap and backoff); `None`: right away.
    pub refresh_at: Option<i64>,
    /// The usage endpoint has blocked the shown account (a 429) until then:
    /// nothing asks it before.
    pub paused_until: Option<i64>,
    /// The shown account is being polled.
    pub polling: bool,
}

/// The daemon state machine.
#[derive(Debug)]
pub struct Engine {
    settings: Settings,
    scheduler: Scheduler,
    activity: Activity,
    attention: AttentionState,
    selector: PlanSelector<PathBuf>,
    accounts: BTreeMap<PathBuf, AccountState>,
    in_flight: Vec<PathBuf>,
    active: Option<PathBuf>,
    rate: RateTracker,
    hooks_installed: bool,
    fresh_poll: bool,
    logged_out: bool,
    last_write: Option<i64>,
    sent_seq: u32,
    sent_a: Option<u8>,
    sent_a_at: i64,
    sent_n: Option<u32>,
    last_event_ts: Option<i64>,
    /// Latest prompt or tool activity that names no configured account.
    unattributed_activity: Option<i64>,
    /// A `claude` binary was found, so the probe can run.
    claude_found: bool,
    /// A new alert should chime on this computer (`desktop_chime`).
    desktop_chime_due: bool,
}

fn utc(now: i64) -> DateTime<Utc> {
    Utc.timestamp_opt(now, 0).single().unwrap_or_default()
}

impl Engine {
    /// A fresh engine; the first `poll_due` fires immediately.
    pub fn new(settings: Settings) -> Self {
        let cadence = Cadence::from_settings(settings.poll_interval, settings.poll_interval_explicit);
        Engine {
            settings,
            scheduler: Scheduler::new(cadence),
            activity: Activity::default(),
            attention: AttentionState::default(),
            selector: PlanSelector::default(),
            accounts: BTreeMap::new(),
            in_flight: Vec::new(),
            active: None,
            rate: RateTracker::default(),
            hooks_installed: false,
            fresh_poll: false,
            logged_out: false,
            last_write: None,
            sent_seq: 0,
            sent_a: None,
            sent_a_at: i64::MIN / 2,
            sent_n: None,
            last_event_ts: None,
            unattributed_activity: None,
            claude_found: true,
            desktop_chime_due: false,
        }
    }

    /// Current settings.
    pub fn settings(&self) -> &Settings {
        &self.settings
    }

    /// Apply re-read settings (the config is re-read every few seconds).
    pub fn update_settings(&mut self, settings: Settings) {
        if settings == self.settings {
            return;
        }
        self.scheduler.cadence = Cadence::from_settings(settings.poll_interval, settings.poll_interval_explicit);
        // A newly added account has never been polled, so it is due at once.
        if settings.config_dirs != self.settings.config_dirs {
            self.accounts.retain(|k, _| settings.config_dirs.contains(k));
        }
        self.settings = settings;
    }

    /// Whether events.log exists.
    pub fn set_hooks_installed(&mut self, installed: bool) {
        self.hooks_installed = installed;
    }

    /// History from events.log at start-up (no `e` is emitted for it).
    pub fn seed_events(&mut self, events: &[HookEvent]) {
        self.activity.seed(events);
    }

    /// One live hook event.
    pub fn on_hook_event(&mut self, ev: &HookEvent, now: i64) {
        if ev.kind.sends_messages() {
            match ev.account.as_ref().and_then(|a| self.settings.config_dirs.iter().find(|d| *d == a)) {
                Some(d) => self.accounts.entry(d.clone()).or_default().note_activity(ev.ts),
                None => self.unattributed_activity = self.unattributed_activity.max(Some(ev.ts)),
            }
        }
        match self.activity.ingest(ev) {
            Signal::Prompt { .. } => {
                self.scheduler.on_prompt(now);
                self.last_event_ts = Some(now);
            }
            Signal::Stop => {
                self.scheduler.on_stop(now);
                self.last_event_ts = Some(now);
            }
            Signal::Tool | Signal::Other => {}
        }
    }

    /// One tick's attention files.
    /// Alerts of a kind the user turned off (`alert_on`) are dropped here,
    /// so neither the device nor the tray sees them.
    pub fn on_attention(&mut self, alert: Option<Alert>, clears: &HashMap<String, SystemTime>) {
        let alert = alert.filter(|a| self.settings.alert_on.allows(a.kind));
        if self.attention.ingest(alert, clears) && self.settings.desktop_chime {
            self.desktop_chime_due = true;
        }
    }

    /// A new alert went up since the last call and `desktop_chime` is on:
    /// play the chime on this computer.
    pub fn take_desktop_chime(&mut self) -> bool {
        std::mem::take(&mut self.desktop_chime_due)
    }

    /// End an alert past its time to live (see [`AttentionState::expire`]).
    pub fn expire_attention(&mut self, now: SystemTime) -> bool {
        self.attention.expire(now)
    }

    /// The device says the user dismissed the alert there (or it timed out).
    pub fn on_device_dismissed(&mut self) {
        self.attention.dismissed();
    }

    /// The device (REQ notify) or the user asked for fresh numbers. Accounts
    /// that are backing off, or were polled in the last 2 minutes, still
    /// wait for that to pass.
    pub fn request_refresh(&mut self, now: i64) {
        self.scheduler.request(now);
    }

    /// A (re)connection: resend everything the device needs.
    pub fn on_connected(&mut self) {
        self.attention.reset();
        self.sent_a = None;
        self.sent_n = None;
        if self.active.is_some() || self.logged_out {
            self.fresh_poll = true;
        }
    }

    /// Whether a `claude` binary was found. Without one the probe can't run,
    /// so the idle source (the usage endpoint) answers on its slower cadence
    /// even while a window runs.
    pub fn set_claude_found(&mut self, found: bool) {
        self.claude_found = found;
    }

    /// The accounts whose usage endpoint is blocked at `now` by a 429, to
    /// save across restarts. Other misses back off in memory only.
    pub fn blocks(&self, now: i64) -> BTreeMap<PathBuf, SavedBlock> {
        self.accounts
            .iter()
            .filter(|(_, a)| a.last_error.as_ref().is_some_and(UsageError::is_throttle))
            .filter_map(|(d, a)| {
                let until = a.backoff.until().filter(|u| *u > now)?;
                Some((d.clone(), SavedBlock { strikes: a.backoff.strikes(), until }))
            })
            .collect()
    }

    /// Restore saved blocks (at start-up). Expired ones are dropped; the
    /// rest keep the endpoint and `claude -p /usage` away from that account
    /// until they pass, whatever asks.
    pub fn restore_blocks(&mut self, blocks: &BTreeMap<PathBuf, SavedBlock>, now: i64) {
        for (d, b) in blocks.iter().filter(|(d, b)| b.until > now && self.settings.config_dirs.contains(d)) {
            let st = self.accounts.entry(d.clone()).or_default();
            st.backoff = crate::backoff::Backoff::restore(b.strikes, b.until);
            st.last_error.get_or_insert(UsageError::Throttled { retry_after_secs: None });
        }
    }

    /// Where this account's next reading would come from at `now`.
    fn source(&self, dir: &PathBuf, now: i64) -> Source {
        let empty = AccountState::default();
        match probe::choose_source(self.accounts.get(dir).unwrap_or(&empty), self.unattributed_activity, now) {
            Source::Probe if !self.claude_found => Source::Usage,
            s => s,
        }
    }

    fn clock(&self, dir: &PathBuf, now: i64) -> AccountClock {
        let st = self.accounts.get(dir);
        let source = self.source(dir, now);
        AccountClock {
            shown: self.active.as_ref().is_none_or(|a| a == dir),
            last_attempt: st.and_then(|a| a.last_attempt_at),
            last_failed: st.is_some_and(|a| a.last_error.is_some()),
            // The backoff is `/usage`'s (its endpoint's rate limit).
            blocked_until: st.and_then(|a| a.backoff.until()).filter(|_| source == Source::Usage),
            usage_only: source == Source::Usage,
            wake_at: st
                .and_then(|a| a.last.as_ref())
                .and_then(|r| r.session.as_ref())
                .and_then(|w| w.resets_at)
                .map(|t| t.timestamp()),
        }
    }

    fn account_due(&self, dir: &PathBuf, now: i64) -> (i64, PollReason) {
        self.scheduler.account_due(&self.clock(dir, now), &self.activity, self.hooks_installed, now)
    }

    fn refresh_at(&self, dir: &PathBuf, now: i64) -> Option<i64> {
        self.scheduler.earliest_poll(&self.clock(dir, now))
    }

    /// The earliest account's next poll: (unix seconds, reason).
    fn next_due(&self, now: i64) -> Option<(i64, PollReason)> {
        self.settings.config_dirs.iter().map(|d| self.account_due(d, now)).min_by_key(|(t, _)| *t)
    }

    /// `Some(reason)` when some account's `/usage` poll is due now.
    pub fn poll_due(&self, now: i64) -> Option<PollReason> {
        self.next_due(now).filter(|(t, _)| *t <= now).map(|(_, why)| why)
    }

    /// Start a poll: the accounts due now, stamped as attempted. `None` when
    /// nothing is due. Hand the results to [`Engine::on_poll_results`].
    pub fn begin_poll(&mut self, now: i64) -> Option<PollPlan> {
        let reason = self.poll_due(now)?;
        let accounts: Vec<PathBuf> =
            self.settings.config_dirs.iter().filter(|d| self.account_due(d, now).0 <= now).cloned().collect();
        let plans = accounts
            .iter()
            .map(|d| match self.source(d, now) {
                Source::Probe => ProbePlan::Probe {
                    usage_fallback: self.accounts.get(d).is_none_or(|a| a.backoff.remaining(now).is_none()),
                },
                Source::Usage => ProbePlan::Usage,
                Source::NotStarted(r) => ProbePlan::Known(r),
            })
            .collect();
        for d in &accounts {
            self.accounts.entry(d.clone()).or_default().last_attempt_at = Some(now);
        }
        self.in_flight = accounts.clone();
        Some(PollPlan { reason, accounts, plans })
    }

    /// Results of one poll (the accounts of a [`PollPlan`]).
    /// A plain `Result` counts as the idle source's.
    pub fn on_poll_results<R: Into<probe::Polled>>(&mut self, now: i64, results: Vec<(PathBuf, R)>) {
        let results: Vec<(PathBuf, probe::Polled)> = results.into_iter().map(|(d, r)| (d, r.into())).collect();
        let polled = !results.is_empty();
        let ok_dirs: Vec<PathBuf> = results.iter().filter(|(_, r)| r.result.is_ok()).map(|(d, _)| d.clone()).collect();
        for (dir, r) in results {
            let st = self.accounts.entry(dir.clone()).or_default();
            // Results without a `begin_poll` (`--once`) count as attempted now.
            if !self.in_flight.contains(&dir) {
                st.last_attempt_at = Some(now);
            }
            st.record_polled(now, r);
            if let (Some(left), Some(e)) = (st.backoff.remaining(now), &st.last_error) {
                log::info!("{}: {e}; not polling it for {left}s", dir.display());
            }
        }
        // Every configured account's latest answer is a login problem.
        let all_auth = !self.settings.config_dirs.is_empty()
            && self
                .settings
                .config_dirs
                .iter()
                .all(|d| self.accounts.get(d).and_then(|a| a.last_error.as_ref()).is_some_and(UsageError::is_auth));
        // Accounts that missed (or weren't polled) keep their last values,
        // which don't count as a rise, so a miss, a rate limit or a backoff
        // never flips the shown account. Only an account with no reading at
        // all is left out.
        let sessions: Vec<(PathBuf, u32)> =
            self.accounts.iter().filter_map(|(k, a)| a.last.as_ref().map(|r| (k.clone(), r.session_pct()))).collect();
        let prev_active = self.active.clone();
        self.active = self.selector.choose(&sessions);
        if self.active != prev_active && self.settings.config_dirs.len() > 1 {
            if let Some(a) = &self.active {
                log::info!("active plan: {}", a.display());
            }
        }
        // Only numbers read in this cycle for the shown account are a real
        // poll. When it missed (another account answered), its last values
        // must not enter the rate math, here or on the device: no `Poll`
        // write, so they would only ride along on a replay.
        let active_fresh = self.active.as_ref().is_some_and(|a| ok_dirs.contains(a));
        if active_fresh {
            if let Some(r) = self.active_reading() {
                let pct = r.session_pct();
                self.rate.sample(now, pct, false);
            }
        }
        self.in_flight.clear();
        self.logged_out = all_auth;
        self.fresh_poll = active_fresh || (all_auth && polled);
    }

    fn active_state(&self) -> Option<&AccountState> {
        self.active.as_ref().and_then(|a| self.accounts.get(a))
    }

    fn active_reading(&self) -> Option<&UsageReading> {
        self.active_state().and_then(|a| a.last.as_ref())
    }

    /// The shown account has missed three polls in a row.
    pub fn stale(&self, now: i64) -> bool {
        self.active_state().is_some_and(|a| a.is_stale(now))
    }

    /// The next write, if one is due. Call [`Engine::mark_written`] after a
    /// successful write; on failure just call this again next tick.
    pub fn next_write(&mut self, now: i64, host: HostClock) -> Option<Write> {
        self.activity.prune(now);
        let level = self.activity.level(now);
        let sessions = self.activity.sessions_active(now);
        let hooks = self.activity.hooks_seen;
        let event_fresh = self.last_event_ts.is_some_and(|t| now - t < EVENT_FRESH_SECS);

        let new_event = event_fresh && self.activity.seq != self.sent_seq;
        let a_changed = hooks && self.sent_a != Some(level) && now - self.sent_a_at >= ACTIVITY_MIN_INTERVAL;
        let n_changed = hooks && self.sent_n != Some(sessions);

        let (mut payload, kind) = if self.logged_out {
            if !(self.fresh_poll || self.attention.pending()) {
                return None;
            }
            (Payload::no_data(), WriteKind::NoData)
        } else {
            // Nothing to show until the first good poll.
            let reading = self.active_reading()?.clone();
            let kind = if self.fresh_poll {
                WriteKind::Poll
            } else if self.attention.pending() {
                WriteKind::Attention
            } else if (new_event || a_changed || n_changed) && !self.stale(now) {
                // Any ok payload refreshes the device's DATA_FRESH_MS, so
                // while the numbers are stale only alerts and polls go out
                // and the device can show its stale state.
                WriteKind::Event
            } else if !self.stale(now) && heartbeat_due(self.last_write, self.settings.heartbeat_interval, now) {
                WriteKind::Heartbeat
            } else {
                return None;
            };
            let mut p = Payload::from_reading(&reading, utc(now));
            if self.settings.reset_chime {
                p.c = Some(1);
            }
            if let Some(cf) = clock_fields(self.settings.clock, utc(now), host.utc_offset_secs, host.prefers_12h) {
                p.t = Some(cf.local_epoch);
                p.tf = Some(cf.hour_format);
            }
            if kind != WriteKind::Poll {
                p.rp = Some(1);
            }
            (p, kind)
        };

        let att = self.attention.fields(self.settings.alert_chime);
        if att.beep {
            payload.b = Some(1);
        }
        payload.m = att.message;
        if att.clear {
            payload.x = Some(1);
        }
        if hooks {
            payload.a = Some(level);
            payload.n = Some(sessions);
            if event_fresh {
                if let Some(code) = self.activity.latest {
                    payload.e = Some(code.as_str().to_owned());
                    payload.es = Some(self.activity.seq);
                }
            }
        }
        Some(Write { payload, kind })
    }

    /// The write from [`Engine::next_write`] reached the device.
    pub fn mark_written(&mut self, now: i64, w: &Write) {
        self.last_write = Some(now);
        self.fresh_poll = false;
        if w.payload.b.is_some() || w.payload.m.is_some() || w.payload.x.is_some() || self.attention.pending() {
            self.attention.mark_sent();
        }
        if let Some(es) = w.payload.es {
            self.sent_seq = es;
        }
        if let Some(a) = w.payload.a {
            if self.sent_a != Some(a) {
                self.sent_a_at = now;
            }
            self.sent_a = Some(a);
        }
        if let Some(n) = w.payload.n {
            self.sent_n = Some(n);
        }
    }

    /// Snapshot for the UI.
    pub fn status(&self, now: i64) -> EngineStatus {
        let st = self.active_state();
        EngineStatus {
            active_account: self.active.clone(),
            reading: st.and_then(|a| a.last.clone()),
            age_secs: st.and_then(|a| a.age(now)),
            stale: self.stale(now),
            logged_out: self.logged_out,
            errors: self
                .accounts
                .iter()
                .filter_map(|(k, a)| {
                    a.last_error.as_ref().map(|e| AccountError {
                        dir: k.clone(),
                        message: match a.backoff.until().filter(|u| *u > now) {
                            Some(until) if e.is_throttle() => {
                                crate::usage::paused_message(until, crate::platform::utc_offset_secs())
                            }
                            _ => e.user_message(a.backoff.remaining(now)),
                        },
                        retrying: e.is_transient(),
                    })
                })
                .collect(),
            attention: self.attention.active(),
            activity: self.activity.level(now),
            sessions: self.activity.sessions_active(now),
            hooks: self.activity.hooks_seen,
            mood: st.and_then(|a| a.last.as_ref()).map(|_| self.rate.mood(self.activity.level(now))),
            next_poll_at: self.next_due(now).map(|(t, _)| t),
            refresh_at: self.active.as_ref().and_then(|a| self.refresh_at(a, now)),
            // The shown account, or the first one while nothing is shown yet.
            paused_until: self
                .active
                .as_ref()
                .or(self.settings.config_dirs.first())
                .and_then(|d| self.accounts.get(d))
                .filter(|a| a.last_error.as_ref().is_some_and(UsageError::is_throttle))
                .and_then(|a| a.backoff.until())
                .filter(|u| *u > now),
            polling: self.active.as_ref().is_some_and(|a| self.in_flight.contains(a)),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::attention::AlertKind;
    use crate::config::{ConfigFile, Settings};
    use crate::events::HookKind;
    use crate::paths::Paths;
    use crate::usage::Window;
    use std::path::Path;
    use std::time::Duration;

    const T0: i64 = 1_760_000_000;

    fn settings(text: &str) -> Settings {
        Settings::from_config(&ConfigFile::parse(text), &Paths::under(Path::new("/h")), None)
    }

    fn reading(s: u32) -> UsageReading {
        UsageReading {
            session: Some(Window { pct: s, resets_at: Some(utc(T0 + 3600)) }),
            week: Some(Window { pct: 10, resets_at: Some(utc(T0 + 86_400)) }),
            ..Default::default()
        }
    }

    fn ev(ts: i64, kind: &str) -> HookEvent {
        HookEvent { ts, kind: HookKind::parse(kind), session: "s1".into(), account: None }
    }

    fn write(e: &mut Engine, now: i64) -> Option<Write> {
        let w = e.next_write(now, HostClock::default())?;
        e.mark_written(now, &w);
        Some(w)
    }

    #[test]
    fn nothing_before_first_poll_then_poll_write() {
        let mut e = Engine::new(settings(""));
        assert_eq!(e.poll_due(T0), Some(PollReason::Startup));
        assert!(write(&mut e, T0).is_none());
        e.on_poll_results(T0, vec![(PathBuf::from("/h/.claude"), Ok(reading(15)))]);
        let w = write(&mut e, T0).unwrap();
        assert_eq!(w.kind, WriteKind::Poll);
        assert_eq!(w.payload.to_json(), r#"{"s":15,"sr":60,"w":10,"wr":1440,"st":"allowed","acct":"pro","ok":true}"#);
        assert!(write(&mut e, T0 + 1).is_none());
    }

    // Heartbeat replays (bash parity + rp marker), and none once stale.
    #[test]
    fn heartbeat_replays_with_aged_countdowns() {
        let mut e = Engine::new(settings("reset_chime = on\n"));
        let dir = PathBuf::from("/h/.claude");
        e.on_poll_results(T0, vec![(dir.clone(), Ok(reading(15)))]);
        write(&mut e, T0).unwrap();
        assert!(write(&mut e, T0 + 59).is_none());
        let hb = write(&mut e, T0 + 300).unwrap();
        assert_eq!(hb.kind, WriteKind::Heartbeat);
        assert_eq!(
            hb.payload.to_json(),
            r#"{"s":15,"sr":55,"w":10,"wr":1435,"st":"allowed","acct":"pro","c":1,"ok":true,"rp":1}"#
        );
        // Three misses: stale, heartbeats stop so the device's freshness lapses.
        for i in 1..=3 {
            e.on_poll_results(T0 + 300 + i, vec![(dir.clone(), Err(UsageError::Timeout))]);
        }
        assert!(e.stale(T0 + 304));
        assert!(write(&mut e, T0 + 1000).is_none());
        assert_eq!(e.status(T0 + 1000).reading, Some(reading(15)), "last values kept");
    }

    #[test]
    fn multi_account_selection_and_glitch_keeps_choice() {
        let mut e = Engine::new(settings("config_dirs = ~/.claude, ~/.claude-work\n"));
        let a = PathBuf::from("/h/.claude");
        let b = PathBuf::from("/h/.claude-work");
        assert_eq!(e.begin_poll(T0).unwrap().accounts, vec![a.clone(), b.clone()]);
        e.on_poll_results(T0, vec![(a.clone(), Ok(reading(10))), (b.clone(), Ok(reading(30)))]);
        assert_eq!(e.status(T0).active_account, Some(b.clone()));
        e.on_poll_results(T0 + 60, vec![(a.clone(), Ok(reading(20))), (b.clone(), Ok(reading(30)))]);
        assert_eq!(e.status(T0).active_account, Some(a.clone()));
        // A misses: still shown with its last values.
        e.on_poll_results(T0 + 120, vec![(a.clone(), Err(UsageError::Timeout)), (b.clone(), Ok(reading(30)))]);
        assert_eq!(e.status(T0).active_account, Some(a.clone()));
        assert_eq!(e.status(T0).reading.unwrap().session_pct(), 20);
        // B tokenless/logged out doesn't matter while A works.
        e.on_poll_results(T0 + 180, vec![(a.clone(), Ok(reading(20))), (b, Err(UsageError::NotLoggedIn("x".into())))]);
        assert!(!e.status(T0 + 180).logged_out);
    }

    // A miss on the shown account is never a poll, even when another
    // account answered: its old values must not reach the rate math.
    #[test]
    fn miss_on_shown_account_is_not_a_poll() {
        let mut e = Engine::new(settings("config_dirs = ~/.claude, ~/.claude-work\n"));
        let a = PathBuf::from("/h/.claude");
        let b = PathBuf::from("/h/.claude-work");
        e.on_poll_results(T0, vec![(a.clone(), Ok(reading(20))), (b.clone(), Ok(reading(5)))]);
        assert_eq!(write(&mut e, T0).unwrap().kind, WriteKind::Poll);
        e.on_poll_results(T0 + 30, vec![(a.clone(), Err(UsageError::Timeout)), (b, Ok(reading(5)))]);
        assert_eq!(e.status(T0 + 30).active_account, Some(a));
        assert!(write(&mut e, T0 + 30).is_none(), "no poll write for the shown account's old values");
        // The keep-alive still carries them, marked as a replay.
        assert_eq!(write(&mut e, T0 + 60).unwrap().payload.rp, Some(1));
    }

    // Stale: hook events stop refreshing the device, so its freshness lapses.
    #[test]
    fn stale_suppresses_event_writes() {
        let mut e = Engine::new(settings(""));
        let dir = PathBuf::from("/h/.claude");
        e.on_poll_results(T0, vec![(dir.clone(), Ok(reading(15)))]);
        write(&mut e, T0).unwrap();
        for i in 1..=3 {
            e.on_poll_results(T0 + i, vec![(dir.clone(), Err(UsageError::Timeout))]);
        }
        e.on_hook_event(&ev(T0 + 10, "UserPromptSubmit"), T0 + 10);
        assert!(write(&mut e, T0 + 10).is_none());
    }

    #[test]
    fn all_logged_out_sends_no_data() {
        let mut e = Engine::new(settings(""));
        e.on_poll_results(T0, vec![(PathBuf::from("/h/.claude"), Err(UsageError::NotLoggedIn("x".into())))]);
        let w = write(&mut e, T0).unwrap();
        assert_eq!((w.kind, w.payload.to_json().as_str()), (WriteKind::NoData, r#"{"ok":false}"#));
        assert!(write(&mut e, T0 + 100).is_none());
    }

    // Ported from test_windows_reconnect.py::test_transient_poll_failure_does_not_set_error
    // and test_freeride.py: only a login problem on every account is "no
    // data"; a glitch (timeout, bad output, CLI missing) stays silent.
    #[test]
    fn transient_failures_are_not_no_data() {
        let mut e = Engine::new(settings("config_dirs = ~/.claude, ~/.claude-work\n"));
        let (a, b) = (PathBuf::from("/h/.claude"), PathBuf::from("/h/.claude-work"));
        e.on_poll_results(
            T0,
            vec![(a.clone(), Err(UsageError::Timeout)), (b.clone(), Err(UsageError::Spawn("x".into())))],
        );
        assert!(!e.status(T0).logged_out);
        assert!(write(&mut e, T0).is_none());
        // One logged out, one glitching: still not "no data".
        e.on_poll_results(
            T0 + 30,
            vec![(a.clone(), Err(UsageError::NotLoggedIn("x".into()))), (b.clone(), Err(UsageError::Timeout))],
        );
        assert!(write(&mut e, T0 + 30).is_none());
        // Both need a login: no data, once.
        e.on_poll_results(
            T0 + 60,
            vec![(a, Err(UsageError::NotSubscription)), (b, Err(UsageError::NotLoggedIn("x".into())))],
        );
        assert_eq!(write(&mut e, T0 + 60).unwrap().kind, WriteKind::NoData);
    }

    // A spend-capped account drives the device's enterprise layout and the
    // multi-account choice by its spend %.
    #[test]
    fn spend_only_account() {
        use crate::usage::Spend;
        let mut e = Engine::new(settings(""));
        let r = UsageReading {
            spend: Some(Spend { pct: 41, resets_at: Some(utc(T0 + 86_400)), period: None }),
            ..Default::default()
        };
        assert_eq!(r.session_pct(), 41);
        e.on_poll_results(T0, vec![(PathBuf::from("/h/.claude"), Ok(r))]);
        let w = write(&mut e, T0).unwrap();
        assert_eq!((w.payload.acct.as_deref(), w.payload.s, w.payload.pd), (Some("ent"), Some(41), Some(30)));
    }

    #[test]
    fn attention_rides_a_replay() {
        let mut e = Engine::new(settings("alert_chime = on\n"));
        e.on_poll_results(T0, vec![(PathBuf::from("/h/.claude"), Ok(reading(15)))]);
        write(&mut e, T0).unwrap();
        let alert = Alert {
            message: "Claude needs you".into(),
            session: "abc".into(),
            kind: AlertKind::Permission,
            at: SystemTime::now(),
        };
        e.on_attention(Some(alert.clone()), &HashMap::new());
        let w = write(&mut e, T0 + 2).unwrap();
        assert_eq!(w.kind, WriteKind::Attention);
        assert_eq!((w.payload.b, w.payload.m.as_deref(), w.payload.rp), (Some(1), Some("Permission needed!"), Some(1)));
        assert!(write(&mut e, T0 + 3).is_none());
        let clears: HashMap<_, _> = [("abc".to_owned(), alert.at + Duration::from_secs(1))].into();
        e.on_attention(None, &clears);
        let w = write(&mut e, T0 + 4).unwrap();
        assert_eq!((w.payload.x, w.payload.m.clone()), (Some(1), None));
    }

    #[test]
    fn stuck_alerts_end_by_expiry_or_device_dismissal() {
        let mut e = Engine::new(settings(""));
        e.on_poll_results(T0, vec![(PathBuf::from("/h/.claude"), Ok(reading(15)))]);
        write(&mut e, T0).unwrap();
        let at = SystemTime::now();
        let alert = |s: &str| Alert { message: "m".into(), session: s.into(), kind: AlertKind::Permission, at };

        // A session killed before its clear: expires with an x=1.
        e.on_attention(Some(alert("gone")), &HashMap::new());
        write(&mut e, T0 + 2).unwrap();
        assert!(!e.expire_attention(at + Duration::from_secs(60)));
        assert!(e.expire_attention(at + crate::attention::ALERT_TTL));
        assert!(!e.status(T0).attention);
        assert_eq!(write(&mut e, T0 + 3).unwrap().payload.x, Some(1));

        // Tapped away on the device: gone here too, nothing sent back.
        e.on_attention(Some(alert(crate::attention::TEST_SESSION)), &HashMap::new());
        write(&mut e, T0 + 4).unwrap();
        e.on_device_dismissed();
        assert!(!e.status(T0).attention);
        assert!(write(&mut e, T0 + 5).is_none());
    }

    #[test]
    fn the_desktop_chime_follows_new_alerts() {
        let at = SystemTime::now();
        let alert = |s: &str, kind| Alert { message: "m".into(), session: s.into(), kind, at };
        let mut e = Engine::new(settings(""));
        e.on_attention(Some(alert("a", AlertKind::Permission)), &HashMap::new());
        assert!(!e.take_desktop_chime(), "off by default");
        let mut e = Engine::new(settings("desktop_chime = on\n"));
        e.on_attention(Some(alert("a", AlertKind::Permission)), &HashMap::new());
        assert!(e.take_desktop_chime());
        assert!(!e.take_desktop_chime(), "once per alert");
        // The same session again (AskUserQuestion notifies twice): no repeat.
        e.on_attention(Some(alert("a", AlertKind::Question)), &HashMap::new());
        assert!(!e.take_desktop_chime());
        // A kind that's turned off never chimes.
        e.on_attention(Some(alert("b", AlertKind::Idle)), &HashMap::new());
        assert!(!e.take_desktop_chime());
        // Cleared in the same tick it arrived: nothing to announce.
        let clears: HashMap<_, _> = [("c".to_owned(), at + Duration::from_secs(1))].into();
        e.on_attention(Some(alert("c", AlertKind::Permission)), &clears);
        assert!(!e.take_desktop_chime());
        // The test alert chimes, so the setting can be tried.
        e.on_attention(Some(alert(crate::attention::TEST_SESSION, AlertKind::Permission)), &HashMap::new());
        assert!(e.take_desktop_chime());
    }

    #[test]
    fn disabled_alert_kinds_never_alert() {
        let mut e = Engine::new(settings("alert_chime = on\n"));
        e.on_poll_results(T0, vec![(PathBuf::from("/h/.claude"), Ok(reading(15)))]);
        write(&mut e, T0).unwrap();
        let idle =
            Alert { message: "waiting".into(), session: "s".into(), kind: AlertKind::Idle, at: SystemTime::now() };
        e.on_attention(Some(idle.clone()), &HashMap::new());
        assert!(!e.status(T0).attention, "idle is off by default");
        assert!(write(&mut e, T0 + 2).is_none());
        e.update_settings(settings("alert_chime = on\nalert_on = idle\n"));
        e.on_attention(Some(idle), &HashMap::new());
        assert!(e.status(T0).attention);
    }

    #[test]
    fn hook_events_reach_the_device() {
        let mut e = Engine::new(settings(""));
        e.set_hooks_installed(true);
        e.on_poll_results(T0, vec![(PathBuf::from("/h/.claude"), Ok(reading(15)))]);
        let w = write(&mut e, T0).unwrap();
        assert_eq!(w.payload.a, None, "no hooks seen yet");

        e.on_hook_event(&ev(T0 + 10, "UserPromptSubmit"), T0 + 10);
        // The prompt's poll waits out the 2-minute gap since the last one.
        assert_eq!(e.poll_due(T0 + 10), None);
        assert_eq!(e.poll_due(T0 + 120), Some(PollReason::Prompt));
        let w = write(&mut e, T0 + 10).unwrap();
        assert_eq!(w.kind, WriteKind::Event, "the event goes out before the poll lands");
        assert_eq!(
            (w.payload.e.as_deref(), w.payload.es, w.payload.n, w.payload.a),
            (Some("p"), Some(1), Some(1), Some(0))
        );
        assert_eq!(w.payload.rp, Some(1));
        e.on_poll_results(T0 + 11, vec![(PathBuf::from("/h/.claude"), Ok(reading(16)))]);
        let w = write(&mut e, T0 + 11).unwrap();
        assert_eq!((w.kind, w.payload.rp, w.payload.s), (WriteKind::Poll, None, Some(16)));

        // Tool calls raise `a`, sent at most every 5 s.
        for i in 0..5 {
            e.on_hook_event(&ev(T0 + 11 + i, "PostToolUse"), T0 + 11 + i);
        }
        assert!(write(&mut e, T0 + 14).is_none());
        let w = write(&mut e, T0 + 15).unwrap();
        assert_eq!(w.payload.a, Some(2));
        // `e` keeps riding along but doesn't trigger writes by itself.
        assert_eq!(w.payload.es, Some(1));

        e.on_hook_event(&ev(T0 + 30, "Stop"), T0 + 30);
        let w = write(&mut e, T0 + 30).unwrap();
        assert_eq!((w.payload.e.as_deref(), w.payload.es), (Some("s"), Some(2)));
        assert_eq!(e.poll_due(T0 + 40), None, "polled at T0+11: gap until T0+131");
        assert_eq!(e.poll_due(T0 + 131), Some(PollReason::AfterStop));

        // Old events stop riding along after a minute.
        let w = e.next_write(T0 + 200, HostClock::default()).unwrap();
        assert_eq!((w.payload.e, w.payload.es), (None, None));
    }

    #[test]
    fn reconnect_resends() {
        let mut e = Engine::new(settings(""));
        e.on_poll_results(T0, vec![(PathBuf::from("/h/.claude"), Ok(reading(15)))]);
        write(&mut e, T0).unwrap();
        e.on_connected();
        assert_eq!(write(&mut e, T0 + 1).unwrap().kind, WriteKind::Poll);
    }

    #[test]
    fn settings_change_triggers_poll() {
        let mut e = Engine::new(settings(""));
        e.on_poll_results(T0, vec![(PathBuf::from("/h/.claude"), Ok(reading(15)))]);
        assert_eq!(e.poll_due(T0 + 1), None);
        e.update_settings(settings("config_dirs = ~/.claude, ~/.w\n"));
        assert_eq!(e.poll_due(T0 + 1), Some(PollReason::Startup));
        assert_eq!(e.begin_poll(T0 + 1).unwrap().accounts, vec![PathBuf::from("/h/.w")], "only the new account");
    }

    fn poll(e: &mut Engine, now: i64, answer: impl Fn(&Path) -> Result<UsageReading, UsageError>) -> Vec<PathBuf> {
        let Some(plan) = e.begin_poll(now) else { return Vec::new() };
        let results = plan.accounts.iter().map(|d| (d.clone(), answer(d))).collect();
        e.on_poll_results(now, results);
        plan.accounts
    }

    fn no_lines() -> Result<UsageReading, UsageError> {
        Err(UsageError::NoUsageLines("You are currently using your subscription".into()))
    }

    /// A reading with no known window (a session line without a reset), so
    /// `/usage` stays the source.
    fn usage_reading(s: u32) -> UsageReading {
        UsageReading { session: Some(Window { pct: s, resets_at: None }), ..reading(s) }
    }

    // The owner's case: the shown account's /usage comes back without its
    // lines (429). It keeps being shown with its last values, isn't polled
    // again for 10 minutes whatever asks, and the other account keeps its
    // own rhythm (12 min while no window is known).
    #[test]
    fn rate_limited_shown_account_backs_off_and_stays() {
        let mut e = Engine::new(settings("config_dirs = ~/.claude, ~/.claude-work\n"));
        e.set_hooks_installed(true);
        let (a, b) = (PathBuf::from("/h/.claude"), PathBuf::from("/h/.claude-work"));
        let pct = |d: &Path| if d == a { 40 } else { 10 };
        assert_eq!(poll(&mut e, T0, |d| Ok(usage_reading(pct(d)))), vec![a.clone(), b.clone()]);
        assert_eq!(e.status(T0).active_account, Some(a.clone()));
        e.request_refresh(T0 + 200);
        assert_eq!(poll(&mut e, T0 + 200, |d| if d == a { no_lines() } else { Ok(usage_reading(10)) }).len(), 2);
        let st = e.status(T0 + 200);
        assert_eq!(st.active_account, Some(a.clone()), "a rate limit doesn't flip the shown account");
        assert_eq!(st.reading.map(|r| r.session_pct()), Some(40));
        assert_eq!(
            st.errors,
            vec![AccountError {
                dir: a.clone(),
                message: "No usage data was returned. Trying again in about 10 minutes.".into(),
                retrying: true,
            }]
        );
        // A refresh request polls only the account that isn't backing off.
        e.request_refresh(T0 + 400);
        assert_eq!(poll(&mut e, T0 + 400, |_| Ok(usage_reading(10))), vec![b.clone()]);
        assert_eq!(e.status(T0 + 400).active_account, Some(a.clone()));
        e.on_hook_event(&ev(T0 + 450, "UserPromptSubmit"), T0 + 450);
        for t in [T0 + 450, T0 + 599, T0 + 799] {
            assert!(e.begin_poll(t).is_none(), "nothing due at {t}");
        }
        assert_eq!(e.status(T0 + 700).next_poll_at, Some(T0 + 800));
        assert_eq!(e.status(T0 + 700).refresh_at, Some(T0 + 800), "a refresh waits for the backoff too");
        // Second miss: 20 minutes.
        assert_eq!(poll(&mut e, T0 + 800, |_| Err(UsageError::RateLimited { retry_after_secs: 400 })), vec![a.clone()]);
        assert!(e.status(T0 + 800).errors[0].message.starts_with("Usage paused by Anthropic until "));
        assert_eq!(poll(&mut e, T0 + 1120, |_| Ok(usage_reading(10))), vec![b.clone()], "B every 12 minutes");
        assert!(e.begin_poll(T0 + 1999).is_none_or(|p| !p.accounts.contains(&a)));
        // Back: errors clear, and the device gets a poll write.
        let polled = poll(&mut e, T0 + 2000, |d| Ok(usage_reading(pct(d) + 5)));
        assert!(polled.contains(&a));
        assert!(e.status(T0 + 2000).errors.is_empty());
        assert_eq!(write(&mut e, T0 + 2000).unwrap().kind, WriteKind::Poll);
    }

    // Without a claude binary there's no probe: the usage endpoint answers
    // every 12 minutes, window or not.
    #[test]
    fn without_claude_the_endpoint_answers_on_its_cadence() {
        let mut e = Engine::new(settings(""));
        e.set_hooks_installed(true);
        e.set_claude_found(false);
        let a = PathBuf::from("/h/.claude");
        e.on_poll_results(T0, vec![(a.clone(), Ok(reading(15)))]);
        let mine = HookEvent { account: Some(a.clone()), ..ev(T0 + 200, "UserPromptSubmit") };
        e.on_hook_event(&mine, T0 + 200);
        assert!(e.begin_poll(T0 + 200).is_none(), "a prompt doesn't speed it up");
        assert_eq!(plans(&mut e, T0 + 720), vec![ProbePlan::Usage]);
    }

    // A block survives a restart: nothing calls the endpoint or /usage for
    // that account until it passes, not even a refresh request.
    #[test]
    fn a_saved_block_holds_after_a_restart() {
        let a = PathBuf::from("/h/.claude");
        let mut e = Engine::new(settings(""));
        e.on_poll_results(T0, vec![(a.clone(), Err(UsageError::Throttled { retry_after_secs: Some(3600) }))]);
        let saved = e.blocks(T0 + 10);
        assert_eq!(saved.get(&a).map(|b| b.until), Some(T0 + 3600));
        let mut fresh = Engine::new(settings(""));
        fresh.restore_blocks(&saved, T0 + 100);
        assert!(fresh.begin_poll(T0 + 100).is_none(), "start-up waits for the block");
        fresh.request_refresh(T0 + 200);
        assert!(fresh.begin_poll(T0 + 200).is_none(), "so does Refresh now");
        assert!(fresh.status(T0 + 200).errors[0].message.starts_with("Usage paused by Anthropic until "));
        assert_eq!(fresh.status(T0 + 200).paused_until, Some(T0 + 3600));
        assert_eq!(fresh.begin_poll(T0 + 3600).map(|p| p.plans), Some(vec![ProbePlan::Usage]));
        // Expired blocks aren't restored.
        let mut later = Engine::new(settings(""));
        later.restore_blocks(&saved, T0 + 4000);
        assert!(later.blocks(T0 + 4000).is_empty());
    }

    fn plans(e: &mut Engine, now: i64) -> Vec<ProbePlan> {
        e.begin_poll(now).map(|p| p.plans).unwrap_or_default()
    }

    // The probe runs only while a window is known to be running, isn't held
    // up by /usage's backoff, and a window that ends quietly needs no call.
    #[test]
    fn the_probe_never_starts_a_window() {
        let mut e = Engine::new(settings(""));
        e.set_hooks_installed(true);
        let a = PathBuf::from("/h/.claude");
        // Unknown at first: /usage.
        assert_eq!(plans(&mut e, T0), vec![ProbePlan::Usage]);
        e.on_poll_results(T0, vec![(a.clone(), Ok(reading(15)))]); // resets at T0 + 3600
                                                                   // Known window: the probe, on the working cadence after a prompt.
        let mine = |ts| HookEvent { account: Some(a.clone()), ..ev(ts, "UserPromptSubmit") };
        e.on_hook_event(&mine(T0 + 200), T0 + 200);
        assert_eq!(plans(&mut e, T0 + 200), vec![ProbePlan::Probe { usage_fallback: true }]);
        // A probe miss with /usage backing off: the probe still runs, alone.
        e.on_poll_results(T0 + 200, vec![(a.clone(), no_lines())]);
        e.on_hook_event(&mine(T0 + 400), T0 + 400);
        assert_eq!(plans(&mut e, T0 + 400), vec![ProbePlan::Probe { usage_fallback: false }]);
        e.on_poll_results(T0 + 400, vec![(a.clone(), Ok(reading(20)))]);
        // Inside the margin before the reset: /usage, which can't start one.
        assert_eq!(plans(&mut e, T0 + 3400), vec![ProbePlan::Usage]);
        e.on_poll_results(T0 + 3400, vec![(a.clone(), Ok(reading(20)))]);
        // At the reset, with nothing since: over, no call.
        assert_eq!(e.status(T0 + 3500).next_poll_at, Some(T0 + 3600), "the reset wakes it");
        let Some(PollPlan { reason, plans: due, .. }) = e.begin_poll(T0 + 3600) else { panic!() };
        assert_eq!(reason, PollReason::WindowEnd);
        let [ProbePlan::Known(r)] = due.as_slice() else { panic!("{due:?}") };
        assert_eq!(r.session, None);
        assert_eq!(r.week, reading(20).week, "weekly kept until its own reset");
        e.on_poll_results(T0 + 3600, vec![(a.clone(), Ok(r.clone()))]);
        assert_eq!(e.status(T0 + 3600).reading.map(|r| r.session_pct()), Some(0));
        // Now /usage keeps watch every 12 min; another account's (or an
        // unknown) prompt doesn't open the probe.
        e.on_hook_event(&ev(T0 + 3700, "UserPromptSubmit"), T0 + 3700);
        assert!(e.begin_poll(T0 + 3700).is_none());
        assert_eq!(plans(&mut e, T0 + 3600 + 720), vec![ProbePlan::Usage]);
        e.on_poll_results(T0 + 4320, vec![(a.clone(), Ok(usage_reading(0)))]);
        // The user's own prompt starts a window: the probe, at once.
        e.on_hook_event(&mine(T0 + 4500), T0 + 4500);
        assert_eq!(plans(&mut e, T0 + 4500), vec![ProbePlan::Probe { usage_fallback: true }]);
    }

    // Only a 429 is saved as a block (and called a pause by Anthropic); a
    // timeout or odd output backs off in memory only.
    #[test]
    fn only_throttles_are_saved_as_blocks() {
        let mut e = Engine::new(settings("config_dirs = ~/.claude, ~/.w\n"));
        let (a, b) = (PathBuf::from("/h/.claude"), PathBuf::from("/h/.w"));
        poll(&mut e, T0, |d| {
            if d == a {
                Err(UsageError::Throttled { retry_after_secs: Some(900) })
            } else {
                Err(UsageError::Timeout)
            }
        });
        let blocks = e.blocks(T0 + 1);
        assert_eq!(blocks.keys().collect::<Vec<_>>(), vec![&a]);
        let errors = e.status(T0 + 1).errors;
        let msg = |d: &PathBuf| errors.iter().find(|x| &x.dir == d).unwrap().message.clone();
        assert!(msg(&a).starts_with("Usage paused by Anthropic until "));
        assert!(!msg(&b).contains("paused"), "{}", msg(&b));
    }

    // An account with no reading at all can't be shown; the other one is.
    #[test]
    fn account_without_data_is_not_shown() {
        let mut e = Engine::new(settings("config_dirs = ~/.claude, ~/.claude-work\n"));
        let (a, b) = (PathBuf::from("/h/.claude"), PathBuf::from("/h/.claude-work"));
        poll(&mut e, T0, |d| if d == a { no_lines() } else { Ok(reading(5)) });
        assert_eq!(e.status(T0).active_account, Some(b));
        assert!(!e.status(T0).logged_out);
        let _ = a;
    }

    // Logged out is about every account's latest answer, not just the ones
    // polled this time.
    #[test]
    fn logged_out_needs_every_account() {
        let mut e = Engine::new(settings("config_dirs = ~/.claude, ~/.claude-work\n"));
        let (a, b) = (PathBuf::from("/h/.claude"), PathBuf::from("/h/.claude-work"));
        e.on_poll_results(T0, vec![(a.clone(), Ok(reading(5))), (b.clone(), Ok(reading(3)))]);
        e.on_poll_results(T0 + 600, vec![(b.clone(), Err(UsageError::NotLoggedIn("x".into())))]);
        assert!(!e.status(T0 + 600).logged_out);
        let errs = e.status(T0 + 600).errors;
        assert_eq!((errs.len(), errs[0].retrying), (1, false));
        e.on_poll_results(T0 + 700, vec![(a, Err(UsageError::NotLoggedIn("x".into())))]);
        assert!(e.status(T0 + 700).logged_out);
    }

    #[test]
    fn clock_fields_included() {
        let mut e = Engine::new(settings("clock = 12\n"));
        e.on_poll_results(T0, vec![(PathBuf::from("/h/.claude"), Ok(reading(15)))]);
        let w = e.next_write(T0, HostClock { utc_offset_secs: -3600, prefers_12h: false }).unwrap();
        assert_eq!((w.payload.t, w.payload.tf), (Some(T0 - 3600), Some(12)));
    }
}
