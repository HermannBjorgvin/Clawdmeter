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
//! * except the stack reporting the daemon's own connects, which the watcher
//!   sees on the shared adapter: a "connected" sighting during an attempt
//!   that reached the device, or within [`OWN_CONNECT_WINDOW`] after it
//!   failed (BlueZ can report the link up and still fail the call), is that
//!   attempt's own and ignored. So a device that connects but then fails
//!   (encryption, discovery) still backs off. An attempt whose connect lost
//!   a race with BlueZ's own reconnect finds the device connected and
//!   carries on, so it needs no retry.

use crate::Sighting;

/// Longest wait between attempts (s). Low enough that a board coming back
/// unnoticed by the wakeup watcher (no BlueZ auto-connect, no scan running)
/// is still picked up quickly; an attempt against an absent device is just a
/// D-Bus listing plus a bounded connect.
pub const BACKOFF_CAP: u64 = 15;

/// Delay after a failed attempt that a wakeup raced (s): long enough that a
/// device which advertises but won't take a connection can't spin the loop.
pub const WOKEN_RETRY: u64 = 1;

/// How long after a failed attempt that reached the device a "connected"
/// sighting still counts as that attempt's own report (s).
pub const OWN_CONNECT_WINDOW: i64 = 3;

/// Reconnect scheduling state.
#[derive(Debug, Clone)]
pub struct Reconnect {
    delay: u64,
    next_at: i64,
    /// What showed up while an attempt was in flight.
    woken: Option<Sighting>,
    /// Until when a "connected" sighting is a failed attempt's own report.
    own_until: Option<i64>,
}

impl Default for Reconnect {
    fn default() -> Self {
        Reconnect { delay: 1, next_at: 0, woken: None, own_until: None }
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

    /// An attempt failed; `reached_device`: it connected to a device, or
    /// tried to ([`crate::LinkError::reached_device`]). Returns the wait (s)
    /// before the next one.
    pub fn failed(&mut self, now: i64, reached_device: bool) -> u64 {
        let raced = match self.woken.take() {
            Some(Sighting::Advertising) => true,
            Some(Sighting::Connected) => !reached_device,
            None => false,
        };
        let wait = if raced {
            self.delay = 1;
            WOKEN_RETRY
        } else {
            let d = self.delay;
            self.delay = (self.delay * 2).min(BACKOFF_CAP);
            d
        };
        self.next_at = now + wait as i64;
        self.own_until = reached_device.then_some(now + OWN_CONNECT_WINDOW);
        wait
    }

    /// An attempt succeeded.
    pub fn connected(&mut self) {
        self.delay = 1;
        self.woken = None;
        self.own_until = None;
    }

    /// The link dropped (or the user asked to reconnect): try again after
    /// `after` seconds, with a fresh backoff.
    pub fn retry_in(&mut self, now: i64, after: u64) {
        self.delay = 1;
        self.woken = None;
        self.own_until = None;
        self.next_at = now + after as i64;
    }

    /// The device showed up. `in_flight`: an attempt is running right now.
    /// True when this brought the next attempt forward.
    pub fn wake(&mut self, now: i64, in_flight: bool, sighting: Sighting) -> bool {
        if sighting == Sighting::Connected && self.own_until.is_some_and(|t| now < t) {
            return false;
        }
        if in_flight {
            // An advertisement is never the attempt's own doing, so it wins.
            if self.woken != Some(Sighting::Advertising) {
                self.woken = Some(sighting);
            }
            return false;
        }
        self.delay = 1;
        let sooner = self.next_at > now;
        self.next_at = self.next_at.min(now);
        sooner
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use Sighting::*;

    /// [`Reconnect::failed`]'s `reached_device`.
    const CONNECTED_TO_IT: bool = true;
    const NOTHING_FOUND: bool = false;

    #[test]
    fn backoff_doubles_to_the_cap() {
        let mut r = Reconnect::default();
        assert!(r.due(0));
        let waits: Vec<u64> = (0..7).map(|_| r.failed(100, CONNECTED_TO_IT)).collect();
        assert_eq!(waits, [1, 2, 4, 8, 15, 15, 15]);
        assert!(!r.due(114));
        assert!(r.due(115));
    }

    #[test]
    fn success_resets() {
        let mut r = Reconnect::default();
        for _ in 0..5 {
            r.failed(0, CONNECTED_TO_IT);
        }
        r.connected();
        assert_eq!(r.failed(10, CONNECTED_TO_IT), 1);
    }

    #[test]
    fn wake_while_idle_makes_the_attempt_due_now() {
        let mut r = Reconnect::default();
        for _ in 0..6 {
            r.failed(100, NOTHING_FOUND);
        }
        assert!(!r.due(101));
        assert!(r.wake(101, false, Connected));
        assert!(r.due(101));
        assert!(!r.wake(101, false, Advertising), "already due");
        // The attempt that wake triggered fails: backoff restarts at 1 s.
        assert_eq!(r.failed(102, CONNECTED_TO_IT), 1);
        assert_eq!(r.failed(103, CONNECTED_TO_IT), 2);
    }

    #[test]
    fn wake_during_an_attempt_is_latched() {
        // The device comes back while an attempt runs, and that attempt
        // fails anyway (it looked before the device was there, or the
        // advertising device wouldn't take the connect): retry almost at
        // once, not after 15 s.
        for (sighting, reached_device) in [(Advertising, CONNECTED_TO_IT), (Connected, NOTHING_FOUND)] {
            let mut r = Reconnect::default();
            for _ in 0..6 {
                r.failed(0, NOTHING_FOUND);
            }
            assert!(!r.wake(200, true, sighting));
            assert_eq!(r.failed(201, reached_device), WOKEN_RETRY, "{sighting:?}");
            assert!(r.due(201 + WOKEN_RETRY as i64));
            // The latch is consumed: the next failure backs off normally.
            assert_eq!(r.failed(203, NOTHING_FOUND), 1);
            assert_eq!(r.failed(204, NOTHING_FOUND), 2);
        }
    }

    #[test]
    fn own_connects_dont_cut_the_backoff() {
        // Each attempt connects and then fails; the watcher reports the
        // device connected during the attempt, or just after it.
        for lag in [None, Some(0), Some(2)] {
            let mut r = Reconnect::default();
            let mut now = 100;
            let mut waits = Vec::new();
            for _ in 0..7 {
                assert!(r.due(now));
                if lag.is_none() {
                    assert!(!r.wake(now, true, Connected));
                }
                let wait = r.failed(now, CONNECTED_TO_IT);
                if let Some(lag) = lag {
                    assert!(!r.wake(now + lag, false, Connected));
                }
                waits.push(wait);
                now += wait as i64;
            }
            assert_eq!(waits, [1, 2, 4, 8, 15, 15, 15], "report lag {lag:?}");
        }
    }

    #[test]
    fn only_reaching_the_device_opens_the_window() {
        let mut r = Reconnect::default();
        for _ in 0..6 {
            r.failed(100, CONNECTED_TO_IT);
        }
        assert!(r.wake(100 + OWN_CONNECT_WINDOW, false, Connected));
        // Nothing was connected to: a "connected" right after is real.
        let mut r = Reconnect::default();
        for _ in 0..6 {
            r.failed(100, NOTHING_FOUND);
        }
        assert!(r.wake(101, false, Connected));
        // Nor does the window outlive a success or a dropped link.
        for clear in [Reconnect::connected as fn(&mut Reconnect), |r| r.retry_in(100, 2)] {
            let mut r = Reconnect::default();
            r.failed(100, CONNECTED_TO_IT);
            clear(&mut r);
            r.failed(100, NOTHING_FOUND);
            assert!(r.wake(100, false, Connected));
        }
    }

    #[test]
    fn success_clears_a_latched_wake() {
        let mut r = Reconnect::default();
        r.wake(0, true, Advertising);
        r.connected();
        assert_eq!(r.failed(5, CONNECTED_TO_IT), 1);
        assert_eq!(r.failed(6, CONNECTED_TO_IT), 2);
    }

    #[test]
    fn wake_never_postpones() {
        let mut r = Reconnect::default();
        r.retry_in(50, 2);
        r.wake(10, false, Advertising);
        assert_eq!(r.next_at(), 10);
    }

    #[test]
    fn retry_in_after_a_drop() {
        let mut r = Reconnect::default();
        for _ in 0..6 {
            r.failed(0, CONNECTED_TO_IT);
        }
        r.retry_in(100, 2);
        assert!(!r.due(101));
        assert!(r.due(102));
        assert_eq!(r.failed(102, CONNECTED_TO_IT), 1);
    }
}
