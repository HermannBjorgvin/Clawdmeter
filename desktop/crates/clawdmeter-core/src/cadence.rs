//! When to poll `/usage`, per account.
//!
//! Every `/usage` run makes Claude Code fetch `GET /api/oauth/usage`, which
//! is rate-limited per account (see [`crate::backoff`]), and Claude Code's
//! own sessions use the same endpoint. So the budget is small:
//!
//! | Situation (shown account)        | /usage                              |
//! |----------------------------------|-------------------------------------|
//! | A prompt was sent                | at once, then every 3 min           |
//! | Just after a Stop                | once, ~10 s later                   |
//! | Between a Stop and 10 quiet min  | every 5 min                         |
//! | No hook events for 10 min        | every 15 min                        |
//! | Device asks on connect (REQ)     | at once                             |
//! | No hooks installed at all        | every 5 min                         |
//!
//! Accounts not shown on the device are polled every 10 min (and on a
//! request); that is how a switch to another plan is noticed. Whatever the
//! trigger, one account is never polled twice within 2 min ([`Cadence::min_gap`]):
//! a prompt, Stop or request inside that gap waits for it to pass. An account
//! backing off after a miss isn't polled at all until its backoff ends, and
//! no request bypasses that. A failure that doesn't back off (CLI missing,
//! logged out) retries after the 2-minute gap.
//!
//! That cadence applies while the account's 5-hour window is known to be
//! running, when the reading comes from the probe ([`crate::probe`]). With
//! no window known to be running, or before the first reading, `/usage` is
//! the source and runs every 12 min ([`Cadence::no_window`]); prompts and
//! Stops don't speed it up. A known reset time wakes the account once, so the
//! device shows the window ending on time.
//!
//! An explicit `poll_interval` in the config replaces the 3- and 5-minute
//! cadences (never below the 2-minute gap), so a user who tuned the old
//! daemon keeps roughly what they had.

use crate::events::Activity;

/// Cadence tunables, in seconds.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Cadence {
    /// Shown account while a session is working.
    pub working: i64,
    /// Delay of the one-shot poll after a Stop.
    pub after_stop: i64,
    /// Quiet time after which the idle cadence applies.
    pub quiet_after: i64,
    /// Idle cadence once hooks have been quiet for `quiet_after`.
    pub quiet: i64,
    /// Cadence when no hooks are installed (and between Stop and quiet).
    pub no_hooks: i64,
    /// Accounts not shown on the device.
    pub other: i64,
    /// Shortest time between two polls of one account, whatever asked.
    pub min_gap: i64,
    /// `/usage` cadence while no window is known to be running.
    pub no_window: i64,
}

impl Default for Cadence {
    fn default() -> Self {
        Cadence {
            working: crate::config::DEFAULT_POLL_INTERVAL as i64,
            after_stop: 10,
            quiet_after: 600,
            quiet: 900,
            no_hooks: 300,
            other: 600,
            min_gap: 120,
            no_window: 720,
        }
    }
}

impl Cadence {
    /// Defaults adjusted by the config's `poll_interval`.
    pub fn from_settings(poll_interval: u64, explicit: bool) -> Self {
        let mut c = Cadence::default();
        if explicit {
            let p = (poll_interval.min(i64::MAX as u64) as i64).max(c.min_gap);
            c.working = p;
            c.no_hooks = p;
        }
        c
    }
}

/// Why a poll is due (for logs and the tray).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PollReason {
    /// The account has never been polled (app start, or newly added).
    Startup,
    /// The device asked (REQ notify) or the user hit "Refresh now".
    Requested,
    /// A prompt was just sent.
    Prompt,
    /// The one-shot ~10 s after a Stop.
    AfterStop,
    /// The regular interval elapsed.
    Interval,
    /// Retrying a failed poll (after its backoff, if it has one).
    Retry,
    /// The account's known 5-hour window just ended.
    WindowEnd,
}

/// What the scheduler needs to know about one account.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct AccountClock {
    /// Shown on the device (or nothing is shown yet).
    pub shown: bool,
    /// When its last poll started.
    pub last_attempt: Option<i64>,
    /// Its last poll failed.
    pub last_failed: bool,
    /// Backing off until then.
    pub blocked_until: Option<i64>,
    /// No window is known to be running: `/usage` on [`Cadence::no_window`],
    /// not sped up by prompts or Stops.
    pub usage_only: bool,
    /// The known window's reset: poll once when it passes.
    pub wake_at: Option<i64>,
}

/// Decides when each account's next `/usage` poll is due. Times are unix
/// seconds. Triggers are stamped with their time; an account has served one
/// once a poll of it started at or after that time.
#[derive(Debug, Clone, Default)]
pub struct Scheduler {
    /// Cadence in use.
    pub cadence: Cadence,
    requested_at: Option<i64>,
    prompt_at: Option<i64>,
    stop_at: Option<i64>,
}

impl Scheduler {
    /// A scheduler; accounts never polled are due at once.
    pub fn new(cadence: Cadence) -> Self {
        Scheduler { cadence, ..Default::default() }
    }

    /// The device or the user asked for fresh numbers.
    pub fn request(&mut self, now: i64) {
        self.requested_at = Some(now);
    }

    /// A prompt was sent.
    pub fn on_prompt(&mut self, now: i64) {
        self.prompt_at = Some(now);
        self.stop_at = None;
    }

    /// A turn stopped at `now`.
    pub fn on_stop(&mut self, now: i64) {
        self.stop_at = Some(now);
    }

    /// The shown account's regular interval for the current situation.
    pub fn interval(&self, activity: &Activity, hooks_installed: bool, now: i64) -> i64 {
        if !hooks_installed && !activity.hooks_seen {
            return self.cadence.no_hooks;
        }
        if activity.working(now) {
            return self.cadence.working;
        }
        match activity.quiet_for(now) {
            Some(q) if q < self.cadence.quiet_after => self.cadence.no_hooks,
            _ => self.cadence.quiet,
        }
    }

    /// The earliest a poll of this account may start, whatever asks for
    /// it: [`Cadence::min_gap`] after its last attempt, and not while it is
    /// backing off.
    pub fn earliest_poll(&self, acct: &AccountClock) -> Option<i64> {
        let gap = acct.last_attempt.map(|t| t + self.cadence.min_gap);
        gap.max(acct.blocked_until)
    }

    /// When this account's next poll is due, and why.
    pub fn account_due(
        &self,
        acct: &AccountClock,
        activity: &Activity,
        hooks_installed: bool,
        now: i64,
    ) -> (i64, PollReason) {
        let blocked = acct.blocked_until.unwrap_or(i64::MIN);
        let Some(last) = acct.last_attempt else { return (now.max(blocked), PollReason::Startup) };
        let interval = if acct.usage_only {
            self.cadence.no_window
        } else if acct.shown {
            self.interval(activity, hooks_installed, now)
        } else {
            self.cadence.other
        };
        let mut best =
            if acct.last_failed { (last, PollReason::Retry) } else { (last + interval, PollReason::Interval) };
        let mut consider = |t: i64, why: PollReason| {
            if t < best.0 {
                best = (t, why);
            }
        };
        if let Some(t) = self.requested_at.filter(|t| *t > last) {
            consider(t, PollReason::Requested);
        }
        if let Some(t) = acct.wake_at.filter(|t| *t > last) {
            consider(t, PollReason::WindowEnd);
        }
        if acct.shown && !acct.usage_only {
            if let Some(t) = self.prompt_at.filter(|t| *t > last) {
                consider(t, PollReason::Prompt);
            }
            if let Some(t) = self.stop_at.filter(|t| *t >= last) {
                consider(t + self.cadence.after_stop, PollReason::AfterStop);
            }
        }
        (best.0.max(last + self.cadence.min_gap).max(blocked), best.1)
    }
}

/// Heartbeat: replay the last payload when nothing was written for
/// `heartbeat_interval` seconds (marked `rp=1`).
pub fn heartbeat_due(last_write: Option<i64>, heartbeat_interval: u64, now: i64) -> bool {
    last_write.is_some_and(|t| now - t >= heartbeat_interval.min(i64::MAX as u64) as i64)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::events::{HookEvent, HookKind};

    fn ev(ts: i64, kind: &str) -> HookEvent {
        HookEvent { ts, kind: HookKind::parse(kind), session: "s".into(), account: None }
    }

    fn shown(last: i64) -> AccountClock {
        AccountClock { shown: true, last_attempt: Some(last), ..Default::default() }
    }

    fn due(s: &Scheduler, acct: &AccountClock, a: &Activity, hooks: bool, now: i64) -> Option<PollReason> {
        let (at, why) = s.account_due(acct, a, hooks, now);
        (at <= now).then_some(why)
    }

    #[test]
    fn startup_and_no_hooks() {
        let a = Activity::default();
        let s = Scheduler::new(Cadence::default());
        assert_eq!(due(&s, &AccountClock::default(), &a, false, 0), Some(PollReason::Startup));
        assert_eq!(due(&s, &shown(0), &a, false, 299), None);
        assert_eq!(due(&s, &shown(0), &a, false, 300), Some(PollReason::Interval));
    }

    #[test]
    fn prompt_then_every_three_minutes_then_stop_once() {
        let mut a = Activity::default();
        let mut s = Scheduler::new(Cadence::default());
        a.ingest(&ev(200, "UserPromptSubmit"));
        s.on_prompt(200);
        assert_eq!(due(&s, &shown(50), &a, true, 200), Some(PollReason::Prompt));
        a.ingest(&ev(230, "PostToolUse"));
        assert_eq!(due(&s, &shown(200), &a, true, 379), None);
        assert_eq!(due(&s, &shown(200), &a, true, 380), Some(PollReason::Interval));
        a.ingest(&ev(600, "Stop"));
        s.on_stop(600);
        assert_eq!(due(&s, &shown(380), &a, true, 609), None);
        assert_eq!(due(&s, &shown(380), &a, true, 610), Some(PollReason::AfterStop));
        // After the stop, not working: 5-min cadence until 10 min quiet.
        assert_eq!(s.account_due(&shown(610), &a, true, 611).0, 910);
        // 10+ minutes since the last event: 15 min cadence.
        assert_eq!(s.account_due(&shown(910), &a, true, 600 + 600).0, 910 + 900);
    }

    // A prompt, Stop or request right after a poll waits out the 2-minute gap.
    #[test]
    fn earliest_poll_follows_gap_and_backoff() {
        let s = Scheduler::default();
        assert_eq!(s.earliest_poll(&AccountClock { shown: true, ..Default::default() }), None);
        assert_eq!(s.earliest_poll(&shown(100)), Some(220));
        let parked = AccountClock { blocked_until: Some(900), ..shown(100) };
        assert_eq!(s.earliest_poll(&parked), Some(900));
    }

    #[test]
    fn min_gap_defers_extra_polls() {
        let mut a = Activity::default();
        let mut s = Scheduler::new(Cadence::default());
        a.ingest(&ev(30, "UserPromptSubmit"));
        s.on_prompt(30);
        assert_eq!(s.account_due(&shown(0), &a, true, 30), (120, PollReason::Prompt));
        a.ingest(&ev(140, "Stop"));
        s.on_stop(140);
        // The prompt poll ran at 120; the Stop at 140 waits until 240, not 150.
        assert_eq!(s.account_due(&shown(120), &a, true, 150), (240, PollReason::AfterStop));
        s.request(241);
        assert_eq!(s.account_due(&shown(240), &a, true, 241), (360, PollReason::Requested));
    }

    #[test]
    fn device_request_is_immediate_once() {
        let a = Activity::default();
        let mut s = Scheduler::new(Cadence::default());
        s.request(500);
        assert_eq!(due(&s, &shown(0), &a, true, 500), Some(PollReason::Requested));
        assert_eq!(due(&s, &shown(500), &a, true, 501), None);
    }

    // Backoff wins over every trigger, including a request.
    #[test]
    fn backoff_blocks_everything() {
        let mut a = Activity::default();
        let mut s = Scheduler::new(Cadence::default());
        let parked = AccountClock {
            shown: true,
            last_attempt: Some(0),
            last_failed: true,
            blocked_until: Some(600),
            ..Default::default()
        };
        s.request(10);
        a.ingest(&ev(20, "UserPromptSubmit"));
        s.on_prompt(20);
        assert_eq!(s.account_due(&parked, &a, true, 20).0, 600);
        assert_eq!(due(&s, &parked, &a, true, 599), None);
        assert!(due(&s, &parked, &a, true, 600).is_some());
        // A failure without backoff (CLI missing) retries after the gap.
        let failed = AccountClock { last_failed: true, ..shown(0) };
        assert_eq!(s.account_due(&failed, &Activity::default(), false, 1), (120, PollReason::Retry));
    }

    #[test]
    fn other_accounts_every_ten_minutes() {
        let mut a = Activity::default();
        let mut s = Scheduler::new(Cadence::default());
        let other = AccountClock { shown: false, last_attempt: Some(0), ..Default::default() };
        a.ingest(&ev(10, "UserPromptSubmit"));
        s.on_prompt(10);
        a.ingest(&ev(20, "Stop"));
        s.on_stop(20);
        assert_eq!(
            s.account_due(&other, &a, true, 30),
            (600, PollReason::Interval),
            "prompts and stops are the shown account's"
        );
        s.request(200);
        assert_eq!(s.account_due(&other, &a, true, 200), (200, PollReason::Requested));
    }

    #[test]
    fn explicit_poll_interval() {
        let c = Cadence::from_settings(300, true);
        assert_eq!((c.working, c.no_hooks, c.quiet), (300, 300, 900));
        assert_eq!(Cadence::from_settings(300, false), Cadence::default());
        let c = Cadence::from_settings(60, true);
        assert_eq!((c.working, c.no_hooks), (120, 120), "never below the gap");
    }

    #[test]
    fn heartbeat_timing() {
        assert!(!heartbeat_due(None, 60, 1000));
        assert!(!heartbeat_due(Some(1000), 60, 1059));
        assert!(heartbeat_due(Some(1000), 60, 1060));
    }

    #[test]
    fn no_known_window_polls_usage_every_12_minutes() {
        let mut s = Scheduler::new(Cadence::default());
        let a = Activity::default();
        let quiet = AccountClock { usage_only: true, ..shown(0) };
        assert_eq!(s.account_due(&quiet, &a, true, 10), (720, PollReason::Interval));
        // Prompts and Stops don't speed it up; a request still does.
        s.on_prompt(30);
        s.on_stop(40);
        assert_eq!(s.account_due(&quiet, &a, true, 50).0, 720);
        s.request(200);
        assert_eq!(s.account_due(&quiet, &a, true, 210), (200, PollReason::Requested));
        // Other accounts too.
        let other = AccountClock { shown: false, ..quiet };
        assert_eq!(Scheduler::new(Cadence::default()).account_due(&other, &a, true, 10).0, 720);
    }

    #[test]
    fn a_known_reset_wakes_the_account() {
        let s = Scheduler::new(Cadence::default());
        let a = Activity::default();
        let c = AccountClock { wake_at: Some(400), ..shown(300) };
        assert_eq!(s.account_due(&c, &a, false, 310), (420, PollReason::WindowEnd), "still after the 2-min gap");
        let c = AccountClock { wake_at: Some(250), ..shown(0) };
        assert_eq!(s.account_due(&c, &a, false, 10), (250, PollReason::WindowEnd));
        // Passed before the last poll: done.
        let c = AccountClock { wake_at: Some(250), ..shown(260) };
        assert_eq!(s.account_due(&c, &a, false, 270).1, PollReason::Interval);
    }
}
