//! What the top of the macOS tray menu shows: the device's home screen in
//! words and numbers (the link, the account, a meter per usage window). Pure,
//! so it's tested on every OS; `mac_menu` draws it.

use super::pace::{self, Level, SESSION_WINDOW_SECS, WEEK_WINDOW_SECS};
use crate::ipc::StatusSnapshot;
use crate::ui::overview::{reset_chips, reset_text, state_title, wait_text, RefreshState, ResetChips};
use crate::ui::widgets::{usage_tone, Tone};
use crate::{ago, tilde};
use clawdmeter_core::usage::Provider;

/// One usage window, laid out like a device panel.
#[derive(Debug, Clone, PartialEq)]
pub struct Meter {
    /// "Session", "Weekly" or "Spend"; a Codex account's main meter is
    /// "Codex" (`tray::meter_labels`).
    pub label: &'static str,
    /// "57%", or "--%" with no reading (no data is never a zero).
    pub value: String,
    /// The bar's fill, 0..=1.
    pub frac: f64,
    /// The fill's colour, as the device picks it (`bar_color` in
    /// firmware/src/ui.cpp): `Ok` green, `Accent` amber, `Bad` red, by the
    /// Overview's thresholds (`usage_tone`) or, with the pace marker on, by
    /// pace (at or behind it, ahead of it, at the limit). `None`: a Codex
    /// account's neutral bar, the Overview's `codex_meter` colour (the
    /// device draws Codex bars white, pace or not).
    pub fill: Option<Tone>,
    /// "Resets in 3d 4h", "Not started", "---".
    pub reset: String,
    /// Where the pace marker sits, 0..=1, when it's on and the reset time
    /// is known.
    pub pace: Option<f64>,
}

/// A weekly-only Codex plan's reset credits, in the second meter's place as
/// on the device and the Overview: "Resets expire", the count beside a
/// ticket, and a chip per held credit's expiry date in one row.
#[derive(Debug, Clone, PartialEq)]
pub struct Resets {
    /// Credits held (the ticket's count; dim at 0).
    pub held: u32,
    /// The row: up to [`RESET_CHIPS`] cells, soonest first, the last one
    /// "+N more" when they don't all fit; or the line in their place.
    pub chips: ResetChips,
}

/// The panel's chips: one row of four across it ("+10 more" fits a chip;
/// a wider "+N more" shortens to "+N", as on the device).
pub const RESET_CHIPS: usize = 4;

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
    /// Session and Weekly; Weekly alone on an account with no 5-hour limit;
    /// Spend alone on a spend-capped account.
    pub meters: Vec<Meter>,
    /// A weekly-only Codex plan's reset credits, under its one meter, while
    /// their count is known (the Linux/Windows menu's `tray::resets_row`).
    pub resets: Option<Resets>,
    /// What the reload button in the corner can do.
    pub refresh: RefreshState,
    /// The footer's speaker: the chime on this computer is on
    /// (`desktop_chime`, the Overview's switch).
    pub chime: bool,
    /// The footer's update icon: what it offers (`tray::update_offer`; the
    /// tray sets it).
    pub update: Option<super::Update>,
    /// Whose endpoint the numbers come from: the paused text names it.
    pub provider: Provider,
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
        if s.spend {
            // A spend-capped account: one meter, its period's reset, and no
            // pace marker (the device's Period bar already is one).
            let reset = reset_text(s.spend_resets_at, now, "---");
            meters.push(meter("Spend", s.session_pct, reset, None, now));
        } else {
            // No 5-hour limit (verified Ultra): the week is the only meter.
            let (session, weekly) = super::meter_labels(s);
            if !s.no_session_limit {
                meters.push(meter(
                    session,
                    s.session_pct,
                    reset_text(s.session_resets_at, now, "Not started"),
                    pace_marker.then_some((s.session_resets_at, SESSION_WINDOW_SECS)),
                    now,
                ));
            }
            meters.push(meter(
                weekly,
                s.week_pct,
                reset_text(s.week_resets_at, now, "---"),
                pace_marker.then_some((s.week_resets_at, WEEK_WINDOW_SECS)),
                now,
            ));
            // A Codex account's bars are neutral, as on the device; the pace
            // marker still shows where the window's time is.
            if s.codex {
                for m in &mut meters {
                    m.fill = None;
                }
            }
        }
        Header {
            title: state_title(s),
            detail,
            age,
            meters,
            resets: resets(s, &chrono::Local),
            refresh,
            chime: desktop_chime,
            update: None,
            provider: super::provider(s),
        }
    }

    /// The reload button's tooltip.
    pub fn refresh_tip(&self) -> String {
        match self.refresh {
            RefreshState::Ready => "Refresh usage".into(),
            RefreshState::Checking => "Checking usage…".into(),
            RefreshState::Wait(secs) => format!("Can check again in {}", wait_text(secs)),
            RefreshState::Paused(until) => clawdmeter_core::usage::paused_message_by(
                self.provider,
                until,
                clawdmeter_core::platform::utc_offset_secs(),
            )
            .trim_end_matches('.')
            .to_owned(),
        }
    }
}

/// The reset credits a weekly-only Codex plan shows (`tray::MenuShape`'s
/// rule), dated in `tz`.
fn resets<Tz: chrono::TimeZone>(s: &StatusSnapshot, tz: &Tz) -> Option<Resets>
where
    Tz::Offset: std::fmt::Display,
{
    let held = s.resets_held.filter(|_| s.codex && s.no_session_limit && !s.spend)?;
    Some(Resets { held, chips: reset_chips(held, &s.resets_expiries, RESET_CHIPS, tz) })
}

/// "14:20", local time: when a pause ends (`usage::paused_clock`).
fn clock(at: i64) -> String {
    clawdmeter_core::usage::paused_clock(at, clawdmeter_core::platform::utc_offset_secs())
}

/// `pace`: the window's reset time and length, with the marker on.
fn meter(label: &'static str, pct: Option<u32>, reset: String, pace: Option<(Option<i64>, i64)>, now: i64) -> Meter {
    let used = pct.unwrap_or(0);
    // No reading, no marker: there's nothing to compare it with.
    let pace = pace.filter(|_| pct.is_some()).and_then(|(resets_at, window)| pace::pace(used, resets_at, window, now));
    let fill = match pace {
        Some((_, Level::Limit)) => Tone::Bad,
        Some((_, Level::Ahead)) => Tone::Accent,
        Some((_, Level::On)) => Tone::Ok,
        None => usage_tone(used),
    };
    let used = f64::from(used);
    Meter {
        label,
        value: pct.map_or_else(|| "--%".to_owned(), |p| format!("{p}%")),
        frac: used.min(100.0) / 100.0,
        fill: Some(fill),
        // With no reading there's no window to describe either.
        reset: if pct.is_some() { reset } else { "---".into() },
        pace: pace.map(|(at, _)| at),
    }
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
        assert_eq!((s.fill, w.fill), (Some(Tone::Ok), Some(Tone::Accent)), "50 and 80 are the device's thresholds");
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
        assert_eq!(at(40).fill, Some(Tone::Ok), "behind the marker");
        assert_eq!(at(51).fill, Some(Tone::Ok), "within a point of it");
        assert_eq!(at(52).fill, Some(Tone::Accent), "ahead of it");
        assert_eq!(at(100).fill, Some(Tone::Bad), "at the limit");
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

    #[test]
    fn a_spend_capped_account_has_one_spend_meter() {
        let s = StatusSnapshot {
            spend: true,
            session_pct: Some(104),
            window_started: false,
            session_resets_at: None,
            week_pct: None,
            week_resets_at: None,
            spend_resets_at: Some(NOW + 16 * 86400),
            ..snap()
        };
        let h = Header::new(&s, Path::new("/home/u"), true, false, RefreshState::Ready, NOW);
        let [m] = h.meters.as_slice() else { panic!("one meter, not a Weekly placeholder") };
        assert_eq!((m.label, m.value.as_str(), m.reset.as_str()), ("Spend", "104%", "Resets in 16d 0h"));
        assert_eq!((m.frac, m.fill, m.pace), (1.0, Some(Tone::Bad), None), "full, red, no pace marker");
    }

    #[test]
    fn a_codex_account_is_labelled_codex_with_neutral_bars() {
        // Weekly-only (the $100 Pro plan): one meter, "Codex".
        let week = StatusSnapshot {
            codex: true,
            account: Some("/home/u/.codex".into()),
            session_pct: None,
            no_session_limit: true,
            week_pct: Some(90),
            week_resets_at: Some(NOW + 3 * 86400),
            ..snap()
        };
        let h = Header::new(&week, Path::new("/home/u"), true, false, RefreshState::Ready, NOW);
        let [m] = h.meters.as_slice() else { panic!("one meter") };
        assert_eq!((m.label, m.value.as_str(), m.fill), ("Codex", "90%", None), "neutral, never the tone colours");
        assert!(m.pace.is_some(), "the device draws the pace marker on Codex bars too");
        // With a 5-hour window: "Codex" and "Weekly", both neutral.
        let five = StatusSnapshot { codex: true, account: Some("/home/u/.codex".into()), ..snap() };
        let h = Header::new(&five, Path::new("/home/u"), false, false, RefreshState::Ready, NOW);
        let labels: Vec<_> = h.meters.iter().map(|m| (m.label, m.fill)).collect();
        assert_eq!(labels, [("Codex", None), ("Weekly", None)]);
        // A pause names OpenAI.
        let paused = Header::new(&five, Path::new("/home/u"), false, false, RefreshState::Paused(NOW + 600), NOW);
        assert!(paused.refresh_tip().starts_with("Usage paused by OpenAI until "));
    }

    #[test]
    fn a_weekly_only_codex_plan_lists_its_resets() {
        let utc = chrono::Utc;
        let day = |m: u32, d: u32| {
            chrono::NaiveDate::from_ymd_opt(2026, m, d).unwrap().and_hms_opt(12, 0, 0).unwrap().and_utc().timestamp()
        };
        let week = StatusSnapshot {
            codex: true,
            account: Some("/home/u/.codex".into()),
            no_session_limit: true,
            week_pct: Some(20),
            resets_held: Some(3),
            resets_expiries: vec![day(11, 30), day(12, 1), day(12, 12)],
            ..snap()
        };
        // Three held: a chip each, soonest first as sent.
        let r = resets(&week, &utc).expect("a weekly-only Codex plan with a count");
        assert_eq!((r.held, r.chips.dates.len(), r.chips.more, r.chips.note), (3, 3, 0, None));
        assert_eq!(r.chips.dates[0], "Nov 30");
        // Ten held: one row of RESET_CHIPS cells, the last "+7 more".
        let ten = StatusSnapshot {
            resets_held: Some(10),
            resets_expiries: (17..27).map(|d| day(11, d)).collect(),
            ..week.clone()
        };
        let r = resets(&ten, &utc).unwrap();
        assert_eq!((r.chips.dates.len(), r.chips.more), (RESET_CHIPS - 1, 7));
        // None held, and a count with no dates: the line in the chips' place.
        let none = StatusSnapshot { resets_held: Some(0), resets_expiries: vec![], ..week.clone() };
        assert_eq!(resets(&none, &utc).unwrap().chips.note, Some("No resets available"));
        let undated = StatusSnapshot { resets_expiries: vec![], ..week.clone() };
        assert_eq!(resets(&undated, &utc).unwrap().chips.note, Some("Expiry dates unknown"));
        // Only where the device and the Linux/Windows menu show them: an
        // unknown count, a 5-hour plan (the week takes that place) or a
        // Claude account has none.
        assert_eq!(resets(&StatusSnapshot { resets_held: None, ..week.clone() }, &utc), None);
        assert_eq!(resets(&StatusSnapshot { no_session_limit: false, ..week.clone() }, &utc), None);
        assert_eq!(resets(&StatusSnapshot { codex: false, ..week.clone() }, &utc), None);
        // Header::new carries them beside the one Codex meter.
        let h = Header::new(&week, Path::new("/home/u"), true, false, RefreshState::Ready, NOW);
        assert_eq!((h.meters.len(), h.resets.map(|r| r.held)), (1, Some(3)));
    }
}
