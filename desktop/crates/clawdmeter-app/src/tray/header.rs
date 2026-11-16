//! What the top of the macOS tray menu shows: the device's home screen in
//! words and numbers (the link, the account, a meter per usage window). Pure,
//! so it's tested on every OS; `mac_menu` draws it.

use crate::ipc::StatusSnapshot;
use crate::ui::overview::{reset_text, state_title, wait_text, RefreshState};
use crate::{ago, tilde};

/// The 5-hour window and the week, in seconds (`PACE_*_MINS` in
/// firmware/src/pace.h).
const SESSION_WINDOW_SECS: i64 = 5 * 3600;
const WEEK_WINDOW_SECS: i64 = 7 * 24 * 3600;
/// Usage within a point of the marker still touches it: on pace
/// (`PACE_GRACE_PCT`).
const PACE_GRACE_PCT: f64 = 1.0;

/// A bar's fill, as the device picks it (`bar_color` in firmware/src/ui.cpp).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Fill {
    /// Under 50 %, or at or behind the pace marker.
    Green,
    /// 50-80 %, or ahead of the pace marker.
    Amber,
    /// From 80 %, or at the limit with the pace marker on.
    Red,
}

/// One usage window, laid out like a device panel.
#[derive(Debug, Clone, PartialEq)]
pub struct Meter {
    /// "Session" or "Weekly".
    pub label: &'static str,
    /// "57%", or "--%" with no reading (no data is never a zero).
    pub value: String,
    /// The bar's fill, 0..=1.
    pub frac: f64,
    /// The fill's colour.
    pub fill: Fill,
    /// "Resets in 3d 4h", "Not started", "---".
    pub reset: String,
    /// Where the pace marker sits, 0..=1, when it's on and the reset time
    /// is known.
    pub pace: Option<f64>,
}

/// The header's content.
#[derive(Debug, Clone, PartialEq)]
pub struct Header {
    /// The link and data state, as the Overview words it ("Connected").
    pub title: &'static str,
    /// The account's folder, or what to do about missing data.
    pub detail: String,
    /// "updated 6 min ago", beside the account (it keeps its room when a long
    /// folder is cut short).
    pub age: Option<String>,
    /// Session and Weekly, or Weekly alone on an account with no 5-hour limit.
    pub meters: Vec<Meter>,
    /// What the reload button in the corner can do.
    pub refresh: RefreshState,
    /// The footer's speaker: the chime on this computer is on
    /// (`desktop_chime`, the Overview's switch).
    pub chime: bool,
    /// The footer's update icon: what it offers (`tray::update_offer`; the
    /// tray sets it).
    pub update: Option<super::Update>,
}

impl Header {
    /// Build from the tray's snapshot. `pace_marker` mirrors the device
    /// setting of that name; `desktop_chime` is the chime on this computer
    /// (the footer's speaker); `refresh` is the reload button's state
    /// (`overview::refresh_state`, a press included).
    pub fn new(
        s: &StatusSnapshot,
        home: &std::path::Path,
        pace_marker: bool,
        desktop_chime: bool,
        refresh: RefreshState,
        now: i64,
    ) -> Self {
        let (detail, age) = match &s.account {
            _ if s.logged_out => ("Open Claude Code to sign in".into(), None),
            // A pause outlasts any check, so it takes the age's place: it says
            // why the numbers are old and until when.
            Some(a) => (
                tilde(a, home),
                match refresh {
                    RefreshState::Paused(until) => Some(format!("paused until {}", clock(until))),
                    _ => s.age(now).map(|age| format!("updated {}", ago(age))),
                },
            ),
            None => ("No usage data yet".into(), None),
        };
        let mut meters = Vec::with_capacity(2);
        // No 5-hour limit (verified Ultra): the week is the only meter.
        if !s.no_session_limit {
            meters.push(meter(
                "Session",
                s.session_pct,
                reset_text(s.session_resets_at, now, "Not started"),
                pace_marker.then_some((s.session_resets_at, SESSION_WINDOW_SECS)),
                now,
            ));
        }
        meters.push(meter(
            "Weekly",
            s.week_pct,
            reset_text(s.week_resets_at, now, "---"),
            pace_marker.then_some((s.week_resets_at, WEEK_WINDOW_SECS)),
            now,
        ));
        Header { title: state_title(s), detail, age, meters, refresh, chime: desktop_chime, update: None }
    }

    /// The reload button's tooltip.
    pub fn refresh_tip(&self) -> String {
        match self.refresh {
            RefreshState::Ready => "Refresh usage".into(),
            RefreshState::Checking => "Checking usage…".into(),
            RefreshState::Wait(secs) => format!("Can check again in {}", wait_text(secs)),
            RefreshState::Paused(until) => format!("Usage paused by Anthropic until {}", clock(until)),
        }
    }
}

/// "14:20", local time.
fn clock(at: i64) -> String {
    let local = at + clawdmeter_core::platform::utc_offset_secs();
    chrono::DateTime::<chrono::Utc>::from_timestamp(local, 0).unwrap_or_default().format("%H:%M").to_string()
}

/// `pace`: the window's reset time and length, with the marker on.
fn meter(label: &'static str, pct: Option<u32>, reset: String, pace: Option<(Option<i64>, i64)>, now: i64) -> Meter {
    let used = pct.unwrap_or(0) as f64;
    let elapsed = pace.and_then(|(resets_at, window)| Some(elapsed_pct(resets_at?, window, now)));
    let fill = match elapsed {
        Some(_) if used >= 100.0 => Fill::Red,
        Some(e) if used > e + PACE_GRACE_PCT => Fill::Amber,
        Some(_) => Fill::Green,
        None if used >= 80.0 => Fill::Red,
        None if used >= 50.0 => Fill::Amber,
        None => Fill::Green,
    };
    Meter {
        label,
        value: pct.map_or_else(|| "--%".to_owned(), |p| format!("{p}%")),
        frac: used.min(100.0) / 100.0,
        fill,
        // With no reading there's no window to describe either.
        reset: if pct.is_some() { reset } else { "---".into() },
        // No reading, no marker: there's nothing to compare it with.
        pace: elapsed.filter(|_| pct.is_some()).map(|e| e / 100.0),
    }
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
    use std::path::Path;

    const NOW: i64 = 1_000_000;

    fn snap() -> StatusSnapshot {
        StatusSnapshot {
            account: Some("/home/u/.claude".into()),
            read_at: Some(NOW - 360),
            session_pct: Some(34),
            week_pct: Some(62),
            window_started: true,
            session_resets_at: Some(NOW + 2 * 3600 + 56 * 60),
            week_resets_at: Some(NOW + (5 * 24 + 11) * 3600),
            ..Default::default()
        }
    }

    #[test]
    fn session_and_weekly_like_the_device() {
        let h = Header::new(&snap(), Path::new("/home/u"), false, false, RefreshState::Ready, NOW);
        assert_eq!((h.detail.as_str(), h.age.as_deref()), ("~/.claude", Some("updated 6 min ago")));
        let [s, w] = h.meters.as_slice() else { panic!("two meters") };
        assert_eq!((s.label, s.value.as_str(), s.reset.as_str()), ("Session", "34%", "Resets in 2h 56m"));
        assert_eq!((w.label, w.value.as_str(), w.reset.as_str()), ("Weekly", "62%", "Resets in 5d 11h"));
        assert_eq!((s.fill, w.fill), (Fill::Green, Fill::Amber), "50 and 80 are the device's thresholds");
        assert_eq!((s.pace, w.pace), (None, None), "no marker unless it's on");
    }

    #[test]
    fn weekly_alone_with_no_session_limit() {
        let s = StatusSnapshot { session_pct: None, no_session_limit: true, week_pct: Some(57), ..snap() };
        let h = Header::new(&s, Path::new("/home/u"), false, false, RefreshState::Ready, NOW);
        assert_eq!(h.meters.len(), 1);
        assert_eq!(h.meters[0].label, "Weekly");
    }

    #[test]
    fn no_reading_is_never_a_zero() {
        let s = StatusSnapshot { session_pct: None, week_pct: None, account: None, read_at: None, ..snap() };
        let h = Header::new(&s, Path::new("/home/u"), true, false, RefreshState::Ready, NOW);
        assert_eq!((h.detail.as_str(), h.age.as_deref()), ("No usage data yet", None));
        assert!(h.meters.iter().all(|m| m.value == "--%" && m.frac == 0.0 && m.pace.is_none() && m.reset == "---"));
    }

    #[test]
    fn a_session_not_started() {
        let s = StatusSnapshot { session_pct: Some(0), window_started: false, session_resets_at: None, ..snap() };
        let h = Header::new(&s, Path::new("/home/u"), true, false, RefreshState::Ready, NOW);
        assert_eq!((h.meters[0].value.as_str(), h.meters[0].reset.as_str()), ("0%", "Not started"));
        assert_eq!(h.meters[0].pace, None, "no reset time, no marker");
    }

    #[test]
    fn signed_out_says_what_to_do() {
        let s = StatusSnapshot { logged_out: true, ..snap() };
        let h = Header::new(&s, Path::new("/home/u"), false, false, RefreshState::Ready, NOW);
        assert_eq!((h.detail.as_str(), h.age.as_deref()), ("Open Claude Code to sign in", None));
    }

    #[test]
    fn the_pace_marker_colours_by_pace() {
        // 2 h 30 m into the 5-hour window: the marker is at half.
        let half = StatusSnapshot { session_resets_at: Some(NOW + 9000), ..snap() };
        let at = |pct| {
            let s = StatusSnapshot { session_pct: Some(pct), ..half.clone() };
            Header::new(&s, Path::new("/home/u"), true, false, RefreshState::Ready, NOW).meters[0].clone()
        };
        assert_eq!(at(40).pace, Some(0.5));
        assert_eq!(at(40).fill, Fill::Green, "behind the marker");
        assert_eq!(at(51).fill, Fill::Green, "within a point of it");
        assert_eq!(at(52).fill, Fill::Amber, "ahead of it");
        assert_eq!(at(100).fill, Fill::Red, "at the limit");
        // A countdown longer than the window is a window just begun.
        let skew = StatusSnapshot { session_resets_at: Some(NOW + 6 * 3600), ..snap() };
        assert_eq!(
            Header::new(&skew, Path::new("/home/u"), true, false, RefreshState::Ready, NOW).meters[0].pace,
            Some(0.0)
        );
    }

    #[test]
    fn the_reload_button_says_what_it_can_do() {
        let h = |r| Header::new(&snap(), Path::new("/home/u"), false, false, r, NOW);
        assert_eq!(h(RefreshState::Ready).refresh_tip(), "Refresh usage");
        assert_eq!(h(RefreshState::Checking).refresh_tip(), "Checking usage…");
        assert_eq!(h(RefreshState::Wait(100)).refresh_tip(), "Can check again in 1 min 40 s");
        assert_eq!(h(RefreshState::Ready).age.as_deref(), Some("updated 6 min ago"));
        let paused = h(RefreshState::Paused(NOW + 600));
        assert!(paused.refresh_tip().starts_with("Usage paused by Anthropic until "));
        assert!(paused.age.as_deref().is_some_and(|a| a.starts_with("paused until ")), "{:?}", paused.age);
    }

    #[test]
    fn the_footer_shows_the_chime() {
        assert!(!Header::new(&snap(), Path::new("/home/u"), false, false, RefreshState::Ready, NOW).chime);
        assert!(Header::new(&snap(), Path::new("/home/u"), false, true, RefreshState::Ready, NOW).chime);
    }
}
