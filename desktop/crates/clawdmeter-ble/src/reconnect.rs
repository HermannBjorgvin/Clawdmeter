//! When to try connecting again: exponential backoff, cut short by wakeups.
//!
//! A pure state machine over unix seconds, so the daemon loop stays a thin
//! driver and the policy is unit-tested. The policy:
//!
//! * a failed attempt waits 1, 2, 4, 8 … s, capped at [`BACKOFF_CAP`];
//! * a success resets the delay;
//! * a wakeup (the OS reports the device connected, or advertising) makes the
//!   next attempt due at once and resets the delay. A wakeup that lands while
//!   an attempt is in flight, or within [`OWN_CONNECT_GRACE`] after one
//!   failed, is ignored: the stack reports the daemon's own connect as the
//!   device connecting, so a device that connects but then fails discovery
//!   would otherwise retry every second instead of backing off.

/// Longest wait between attempts (s). Low enough that a board coming back
/// unnoticed by the wakeup watcher (no BlueZ auto-connect, no scan running)
/// is still picked up quickly; an attempt against an absent device is just a
/// D-Bus listing plus a bounded connect.
pub const BACKOFF_CAP: u64 = 15;

/// How long after a failed attempt a wakeup still counts as that attempt's
/// own connect (s). The daemon only looks at both on its 0.5 s tick, in
/// either order, and the stack's report can trail the failure.
pub const OWN_CONNECT_GRACE: i64 = 2;

/// Reconnect scheduling state.
#[derive(Debug, Clone)]
pub struct Reconnect {
    delay: u64,
    next_at: i64,
    failed_at: Option<i64>,
}

impl Default for Reconnect {
    fn default() -> Self {
        Reconnect { delay: 1, next_at: 0, failed_at: None }
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
        let wait = self.delay;
        self.delay = (self.delay * 2).min(BACKOFF_CAP);
        self.next_at = now + wait as i64;
        self.failed_at = Some(now);
        wait
    }

    /// An attempt succeeded.
    pub fn connected(&mut self) {
        self.delay = 1;
    }

    /// The link dropped (or the user asked to reconnect): try again after
    /// `after` seconds, with a fresh backoff.
    pub fn retry_in(&mut self, now: i64, after: u64) {
        self.delay = 1;
        self.next_at = now + after as i64;
    }

    /// The device showed up. `in_flight`: an attempt is running right now.
    /// Ignored then, and just after an attempt failed: the sighting is most
    /// likely that attempt's own connect.
    pub fn wake(&mut self, now: i64, in_flight: bool) {
        if in_flight || self.failed_at.is_some_and(|t| now - t < OWN_CONNECT_GRACE) {
            return;
        }
        self.delay = 1;
        self.next_at = self.next_at.min(now);
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
        assert!(!r.due(102));
        r.wake(102, false);
        assert!(r.due(102));
        // The attempt that wake triggered fails: backoff restarts at 1 s.
        assert_eq!(r.failed(103), 1);
        assert_eq!(r.failed(104), 2);
    }

    #[test]
    fn own_connects_dont_cut_the_backoff() {
        // Each attempt connects at the link layer (the watcher reports the
        // device connected) and then fails, e.g. in service discovery. The
        // daemon sees that report during the attempt, in the same tick as
        // the failure, or a second after it.
        for lag in [None, Some(0), Some(1)] {
            let mut r = Reconnect::default();
            let mut now = 0;
            let mut waits = Vec::new();
            for _ in 0..7 {
                assert!(r.due(now));
                if lag.is_none() {
                    r.wake(now, true);
                }
                let wait = r.failed(now);
                if let Some(lag) = lag {
                    r.wake(now + lag, false);
                }
                waits.push(wait);
                now += wait as i64;
            }
            assert_eq!(waits, [1, 2, 4, 8, 15, 15, 15], "report lag {lag:?}");
        }
    }

    #[test]
    fn wake_never_postpones() {
        let mut r = Reconnect::default();
        r.retry_in(50, 2);
        r.wake(10, false);
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
