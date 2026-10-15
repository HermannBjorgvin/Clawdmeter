//! The slow-clock mood, for the tray and for
//! anyone who wants to mirror what the firmware computes.
//!
//! * 15-minute window over real polls only (replays are ignored).
//! * Expressed in %/h: a 5-hour window used evenly is 20 %/h.
//!   Idle < 5, Normal 5-10, Active 10-20, Heavy > 20.
//! * The hook activity level can lift the mood, never lower it.
//! * A reset is a drop of more than 5 points between real polls.

use std::collections::VecDeque;

/// Rate window.
pub const WINDOW_SECS: i64 = 15 * 60;
/// Drop (in points) that counts as a session reset.
pub const RESET_DROP: u32 = 5;

/// Mood groups, ordered from calm to busy.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub enum Mood {
    /// < 5 %/h.
    Idle,
    /// 5-10 %/h.
    Normal,
    /// 10-20 %/h.
    Active,
    /// > 20 %/h (at pace or faster).
    Heavy,
}

impl Mood {
    /// Group for a rate in %/h.
    pub fn from_rate(pct_per_hour: f64) -> Self {
        if pct_per_hour > 20.0 {
            Mood::Heavy
        } else if pct_per_hour >= 10.0 {
            Mood::Active
        } else if pct_per_hour >= 5.0 {
            Mood::Normal
        } else {
            Mood::Idle
        }
    }

    /// Floor from the hook activity level (0-3).
    pub fn from_activity(level: u8) -> Self {
        match level {
            0 => Mood::Idle,
            1 => Mood::Normal,
            2 => Mood::Active,
            _ => Mood::Heavy,
        }
    }
}

/// Samples of session % from real polls.
#[derive(Debug, Clone, Default)]
pub struct RateTracker {
    samples: VecDeque<(i64, u32)>,
}

/// What a new sample meant.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SampleEvent {
    /// Nothing special.
    None,
    /// The session % dropped by more than [`RESET_DROP`]: a reset.
    Reset,
}

impl RateTracker {
    /// Add a sample. `replay` samples (heartbeats) are ignored.
    pub fn sample(&mut self, now: i64, pct: u32, replay: bool) -> SampleEvent {
        if replay {
            return SampleEvent::None;
        }
        let mut ev = SampleEvent::None;
        if let Some(&(_, last)) = self.samples.back() {
            if last > pct && last - pct > RESET_DROP {
                ev = SampleEvent::Reset;
                self.samples.clear();
            }
        }
        self.samples.push_back((now, pct));
        while self.samples.front().is_some_and(|(t, _)| now - t > WINDOW_SECS) {
            self.samples.pop_front();
        }
        ev
    }

    /// %/h over the window (oldest to newest sample), 0 with < 2 samples.
    pub fn rate(&self) -> f64 {
        match (self.samples.front(), self.samples.back()) {
            (Some(&(t0, p0)), Some(&(t1, p1))) if t1 > t0 => {
                (p1 as f64 - p0 as f64).max(0.0) * 3600.0 / (t1 - t0) as f64
            }
            _ => 0.0,
        }
    }

    /// Mood from the rate, lifted by the hook activity level.
    pub fn mood(&self, activity_level: u8) -> Mood {
        Mood::from_rate(self.rate()).max(Mood::from_activity(activity_level))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn one_point_in_fifteen_minutes_is_four_per_hour() {
        let mut r = RateTracker::default();
        r.sample(0, 10, false);
        r.sample(900, 11, false);
        assert!((r.rate() - 4.0).abs() < 1e-9);
        assert_eq!(r.mood(0), Mood::Idle);
        r.sample(900, 13, false);
        assert_eq!(Mood::from_rate(r.rate()), Mood::Active); // 3 pts/15 min = 12 %/h
    }

    #[test]
    fn replays_ignored_and_activity_lifts() {
        let mut r = RateTracker::default();
        r.sample(0, 10, false);
        r.sample(60, 50, true);
        assert_eq!(r.rate(), 0.0);
        assert_eq!(r.mood(3), Mood::Heavy);
        assert_eq!(r.mood(0), Mood::Idle);
    }

    #[test]
    fn reset_detection() {
        let mut r = RateTracker::default();
        r.sample(0, 80, false);
        assert_eq!(r.sample(60, 76, false), SampleEvent::None);
        assert_eq!(r.sample(120, 0, false), SampleEvent::Reset);
        assert_eq!(r.sample(180, 0, true), SampleEvent::None);
    }

    #[test]
    fn groups() {
        assert_eq!(Mood::from_rate(4.9), Mood::Idle);
        assert_eq!(Mood::from_rate(5.0), Mood::Normal);
        assert_eq!(Mood::from_rate(10.0), Mood::Active);
        assert_eq!(Mood::from_rate(20.0), Mood::Active);
        assert_eq!(Mood::from_rate(20.1), Mood::Heavy);
    }
}
