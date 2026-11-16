//! The pace marker's maths, as the device does it (firmware/src/pace.cpp):
//! where usage would sit if it were spread evenly over the window, and how
//! the reading compares. The macOS panel draws it on the bars; the
//! Linux/Windows rows say it in words.

/// The 5-hour window and the week, in seconds (`PACE_*_MINS` in
/// firmware/src/pace.h).
pub const SESSION_WINDOW_SECS: i64 = 5 * 3600;
/// See [`SESSION_WINDOW_SECS`].
pub const WEEK_WINDOW_SECS: i64 = 7 * 24 * 3600;
/// Usage within a point of the marker still touches it: on pace
/// (`PACE_GRACE_PCT`).
const GRACE_PCT: f64 = 1.0;

/// How a reading compares with the elapsed share of its window
/// (`PaceLevel`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Level {
    /// At or behind the marker.
    On,
    /// Ahead of it.
    Ahead,
    /// At the limit.
    Limit,
}

impl Level {
    /// The words a usage row ends with.
    pub fn words(self) -> &'static str {
        match self {
            Level::On => "on pace",
            Level::Ahead => "ahead of pace",
            Level::Limit => "at the limit",
        }
    }
}

/// Where the marker sits (0..=1) and how `used` % compares, for a window of
/// `window` seconds resetting at `resets_at`. `None` with no reset time.
pub fn pace(used: u32, resets_at: Option<i64>, window: i64, now: i64) -> Option<(f64, Level)> {
    let elapsed = elapsed_pct(resets_at?, window, now);
    let used = f64::from(used);
    let level = if used >= 100.0 {
        Level::Limit
    } else if used > elapsed + GRACE_PCT {
        Level::Ahead
    } else {
        Level::On
    };
    Some((elapsed / 100.0, level))
}

/// The share of a window already elapsed, 0..=100 (`pace_elapsed_pct`). A
/// countdown past the window's length (clock skew) is a window just begun.
fn elapsed_pct(resets_at: i64, window: i64, now: i64) -> f64 {
    let left = (resets_at - now).clamp(0, window);
    100.0 * (window - left) as f64 / window as f64
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn like_the_device() {
        let now = 1_000_000;
        // 2 h 30 m into the 5-hour window: the marker is at half.
        let half = Some(now + 9000);
        assert_eq!(pace(40, half, SESSION_WINDOW_SECS, now), Some((0.5, Level::On)), "behind the marker");
        assert_eq!(pace(51, half, SESSION_WINDOW_SECS, now).map(|p| p.1), Some(Level::On), "within a point");
        assert_eq!(pace(52, half, SESSION_WINDOW_SECS, now).map(|p| p.1), Some(Level::Ahead));
        assert_eq!(pace(100, half, SESSION_WINDOW_SECS, now).map(|p| p.1), Some(Level::Limit));
        assert_eq!(pace(40, None, SESSION_WINDOW_SECS, now), None, "no reset time, no marker");
        // A countdown longer than the window is a window just begun.
        assert_eq!(pace(0, Some(now + 6 * 3600), SESSION_WINDOW_SECS, now).map(|p| p.0), Some(0.0));
    }
}
