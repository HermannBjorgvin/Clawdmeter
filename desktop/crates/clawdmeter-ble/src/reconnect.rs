//! When to try connecting again: exponential backoff, cut short by wakeups.
//!
//! A pure state machine over unix seconds, so the daemon loop stays a thin
//! driver and the policy is unit-tested. The policy:
//!
//! * a failed attempt waits 1, 2, 4, 8 … s, capped at [`BACKOFF_CAP`];
//! * a success resets the delay;
//! * a wakeup (the OS reports the device connected, or advertising) makes the
//!   next attempt due at once and resets the delay. A wakeup that lands while
//!   an attempt is in flight is latched: if that attempt fails (it raced the
//!   device coming back), the retry follows after [`WOKEN_RETRY`] instead of
//!   the backoff;
//! * except the stack's report of the daemon's own connect: a "connected"
//!   sighting while an attempt is in flight, or within
//!   [`OWN_CONNECT_WINDOW`] after one failed (BlueZ can report the link up
//!   and fail the call), is ignored, so a device that connects but then
//!   fails discovery still backs off. A real comeback in that window waits
//!   for the backoff.

use crate::Sighting;

/// Longest wait between attempts (s). Low enough that a board coming back
/// unnoticed by the wakeup watcher (no BlueZ auto-connect, no scan running)
/// is still picked up quickly; an attempt against an absent device is just a
/// D-Bus listing plus a bounded connect.
pub const BACKOFF_CAP: u64 = 15;

/// Delay after a failed attempt that a wakeup raced (s): long enough that a
/// device which advertises but won't take a connection can't spin the loop.
pub const WOKEN_RETRY: u64 = 1;

/// How long after a failed attempt a "connected" sighting still counts as
/// that attempt's own (s).
pub const OWN_CONNECT_WINDOW: i64 = 3;

/// Reconnect scheduling state.
#[derive(Debug, Clone)]
pub struct Reconnect {
    delay: u64,
    next_at: i64,
    woken: bool,
    failed_at: Option<i64>,
}

impl Default for Reconnect {
    fn default() -> Self {
        Reconnect { delay: 1, next_at: 0, woken: false, failed_at: None }
    }
}

impl Reconnect {
    /// An attempt may start now.
    pub fn due(&self, now: i64) -> bool {
        now >= self.next_at
    }

    /// Unix second the next attempt is due.
    pub fn next_at(&self) -> i64 {
        self.next_at
    }

    /// An attempt failed. Returns the wait (s) before the next one.
    pub fn failed(&mut self, now: i64) -> u64 {
        let wait = if std::mem::take(&mut self.woken) {
            self.delay = 1;
            WOKEN_RETRY
        } else {
            let d = self.delay;
            self.delay = (self.delay * 2).min(BACKOFF_CAP);
            d
        };
        self.next_at = now + wait as i64;
        self.failed_at = Some(now);
        wait
    }

    /// An attempt succeeded.
    pub fn connected(&mut self) {
        self.delay = 1;
        self.woken = false;
    }

    /// The link dropped (or the user asked to reconnect): try again after
    /// `after` seconds, with a fresh backoff.
    pub fn retry_in(&mut self, now: i64, after: u64) {
        self.delay = 1;
        self.woken = false;
        self.next_at = now + after as i64;
    }

    /// The device showed up. `in_flight`: an attempt is running right now.
    /// True when this brought the next attempt forward.
    pub fn wake(&mut self, now: i64, in_flight: bool, sighting: Sighting) -> bool {
        let just_failed = self.failed_at.is_some_and(|t| now - t < OWN_CONNECT_WINDOW);
        if sighting == Sighting::Connected && (in_flight || just_failed) {
            return false;
        }
        self.delay = 1;
        if in_flight {
            self.woken = true;
            return false;
        }
        let sooner = self.next_at > now;
        self.next_at = self.next_at.min(now);
        sooner
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn backoff_doubles_to_the_cap() {
        let mut r = Reconnect::default();
        assert!(r.due(0));
        let waits: Vec<u64> = (0..7).map(|_| r.failed(100)).collect();
        assert_eq!(waits, [1, 2, 4, 8, 15, 15, 15]);
        assert!(!r.due(114));
        assert!(r.due(115));
    }

    #[test]
    fn success_resets() {
        let mut r = Reconnect::default();
        for _ in 0..5 {
            r.failed(0);
        }
        r.connected();
        assert_eq!(r.failed(10), 1);
    }

    #[test]
    fn wake_while_idle_makes_the_attempt_due_now() {
        let mut r = Reconnect::default();
        for _ in 0..6 {
            r.failed(100);
        }
        assert!(!r.due(101));
        assert!(r.wake(101, false, Sighting::Advertising));
        assert!(r.due(101));
        assert!(!r.wake(101, false, Sighting::Advertising), "already due");
        // The attempt that wake triggered fails: backoff restarts at 1 s.
        assert_eq!(r.failed(102), 1);
        assert_eq!(r.failed(103), 2);
    }

    #[test]
    fn wake_during_an_attempt_is_latched() {
        let mut r = Reconnect::default();
        for _ in 0..6 {
            r.failed(0);
        }
        // An attempt is running (started when due); the device comes back.
        assert!(!r.wake(200, true, Sighting::Advertising));
        // That attempt fails anyway: retry almost at once, not after 15 s.
        assert_eq!(r.failed(201), WOKEN_RETRY);
        assert!(r.due(201 + WOKEN_RETRY as i64));
        // The latch is consumed: the next failure backs off normally.
        assert_eq!(r.failed(203), 1);
        assert_eq!(r.failed(204), 2);
    }

    #[test]
    fn own_connects_dont_cut_the_backoff() {
        // Each attempt connects at the link layer and then fails (discovery,
        // or BlueZ failing the call): the watcher reports the device
        // connected during the attempt or just after it.
        for lag in [None, Some(0), Some(2)] {
            let mut r = Reconnect::default();
            let mut now = 100;
            let mut waits = Vec::new();
            for _ in 0..7 {
                assert!(r.due(now));
                if lag.is_none() {
                    assert!(!r.wake(now, true, Sighting::Connected));
                }
                let wait = r.failed(now);
                if let Some(lag) = lag {
                    assert!(!r.wake(now + lag, false, Sighting::Connected));
                }
                waits.push(wait);
                now += wait as i64;
            }
            assert_eq!(waits, [1, 2, 4, 8, 15, 15, 15], "report lag {lag:?}");
        }
    }

    #[test]
    fn a_comeback_after_the_window_cuts_the_backoff() {
        let mut r = Reconnect::default();
        for _ in 0..6 {
            r.failed(100);
        }
        assert!(r.wake(100 + OWN_CONNECT_WINDOW, false, Sighting::Connected));
        assert!(r.due(100 + OWN_CONNECT_WINDOW));
        // Advertising is never the daemon's own doing.
        let mut r = Reconnect::default();
        r.failed(100);
        assert!(!r.wake(100, true, Sighting::Advertising));
        assert_eq!(r.failed(101), WOKEN_RETRY);
    }

    #[test]
    fn success_clears_a_latched_wake() {
        let mut r = Reconnect::default();
        r.wake(0, true, Sighting::Advertising);
        r.connected();
        assert_eq!(r.failed(5), 1);
        assert_eq!(r.failed(6), 2);
    }

    #[test]
    fn wake_never_postpones() {
        let mut r = Reconnect::default();
        r.retry_in(50, 2);
        r.wake(10, false, Sighting::Advertising);
        assert_eq!(r.next_at(), 10);
    }

    #[test]
    fn retry_in_after_a_drop() {
        let mut r = Reconnect::default();
        for _ in 0..6 {
            r.failed(0);
        }
        r.retry_in(100, 2);
        assert!(!r.due(101));
        assert!(r.due(102));
        assert_eq!(r.failed(102), 1);
    }
}
