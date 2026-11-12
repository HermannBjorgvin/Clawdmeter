//! Overview: what the tray menu shows, as a page. The link, the live
//! meters, a refresh button, and a nudge when new firmware is out.

use clawdmeter_core::{Paths, Settings};
use clawdmeter_flash::release::release_tag;
use gpui::{div, prelude::*, px, relative, rgb, Context, EventEmitter, Rgba, SharedString, Window};

use super::theme::{Theme, FONT_BODY, FONT_DISPLAY, R_M, R_PILL};
use super::widgets::{
    button, clawd, dot, field, icon_button, notice, toggle, tone_color, usage_tone, ButtonKind, Tone,
};
use super::{command, live_status, not_running_text, Navigate, REFRESH};
use crate::art::TrayState;
use crate::ipc::{AppCommand, StatusSnapshot};
use crate::{ago, tilde, Page};

/// The overview page.
pub struct OverviewView {
    paths: Paths,
    snap: Option<StatusSnapshot>,
    /// When the refresh button was pressed, until that check has run.
    refreshing: Option<i64>,
    /// `desktop_chime` in the config.
    desktop_chime: bool,
    /// `skipped_firmware` in the config: the release the firmware nudge
    /// stays quiet about.
    skipped_firmware: Option<String>,
    /// "Skip this update" couldn't be saved.
    skip_failed: bool,
}

impl EventEmitter<Navigate> for OverviewView {}

impl OverviewView {
    /// Build and start following the tray process's status.
    pub fn new(paths: Paths, cx: &mut Context<Self>) -> Self {
        cx.spawn(async move |this, cx| loop {
            cx.background_executor().timer(REFRESH).await;
            if this.update(cx, |v, cx| v.refresh(cx)).is_err() {
                break;
            }
        })
        .detach();
        let mut v = OverviewView {
            paths,
            snap: None,
            refreshing: None,
            desktop_chime: false,
            skipped_firmware: None,
            skip_failed: false,
        };
        v.reload(clawdmeter_core::unix_now());
        v
    }

    fn reload(&mut self, now: i64) {
        self.snap = live_status(&self.paths);
        if let Ok(s) = Settings::load(&self.paths) {
            self.desktop_chime = s.desktop_chime;
            self.skipped_firmware = s.skipped_firmware;
        }
        if let Some(at) = self.refreshing {
            let done = match &self.snap {
                None => true,
                // The check ran (fresh numbers or a new error) or never will.
                Some(s) => {
                    s.read_at.is_some_and(|r| r >= at)
                        || (!s.polling && now - at > REFRESH_SETTLE_SECS)
                        || now - at > REFRESH_GIVE_UP_SECS
                }
            };
            if done {
                self.refreshing = None;
            }
        }
    }

    fn refresh(&mut self, cx: &mut Context<Self>) {
        let before = self.snap.clone();
        self.reload(clawdmeter_core::unix_now());
        // "updated N s ago" ticks, so redraw while running.
        if self.snap.is_some() || before.is_some() {
            cx.notify();
        }
    }

    /// New firmware is out for the connected device, and not skipped: the
    /// Overview nudges about it and the Flash item carries a badge. (The
    /// Device page offers it either way.)
    pub fn update_available(&self) -> bool {
        self.firmware_nudge().is_some()
    }

    /// The firmware release to nudge about, as a plain tag (`v2.5.0`),
    /// unless it was skipped. The version comes from the release's manifest,
    /// and a skip writes it to the config, so anything but a plain version
    /// is no release to nudge about.
    fn firmware_nudge(&self) -> Option<String> {
        let latest = release_tag(self.snap.as_ref()?.update_available()?)?;
        let skipped = self.skipped_firmware.as_deref().and_then(release_tag);
        (skipped.as_ref() != Some(&latest)).then_some(latest)
    }

    /// Stop nudging about firmware release `tag`; a later one nudges again.
    fn skip_firmware(&mut self, tag: String, cx: &mut Context<Self>) {
        let saved = tag.clone();
        match super::settings::write_settings(&self.paths, move |s, _| s.skipped_firmware = Some(saved)) {
            Ok(()) => {
                self.skipped_firmware = Some(tag);
                self.skip_failed = false;
            }
            Err(e) => {
                log::warn!("couldn't save skipped_firmware: {e}");
                self.skip_failed = true;
            }
        }
        cx.notify();
    }
}

/// After a refresh press, how long to wait for the poll to start.
const REFRESH_SETTLE_SECS: i64 = 3;
/// A `/usage` poll times out well before this.
const REFRESH_GIVE_UP_SECS: i64 = 60;

/// "1 min 40 s" for a wait.
fn wait_text(secs: i64) -> String {
    match secs.max(1) {
        s if s < 60 => format!("{s} s"),
        s if s < 3600 && s % 60 == 0 => format!("{} min", s / 60),
        s if s < 3600 => format!("{} min {} s", s / 60, s % 60),
        s => format!("{} h {} min", s / 3600, (s % 3600) / 60),
    }
}

/// What the refresh button can do right now.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum RefreshState {
    /// A check of the shown account is running.
    Checking,
    /// The account was checked too recently, or is backing off after a
    /// rate limit: a press would only queue a check for then.
    Wait(i64),
    /// A press checks right away.
    Ready,
    /// The usage endpoint blocked the shown account until then (unix s):
    /// nothing asks it before, a press included.
    Paused(i64),
}

/// The refresh button's state for `s`; `pressed` is a press not yet served.
pub fn refresh_state(s: &StatusSnapshot, pressed: bool, now: i64) -> RefreshState {
    if s.polling {
        return RefreshState::Checking;
    }
    if let Some(until) = s.paused_until.filter(|u| *u > now) {
        return RefreshState::Paused(until);
    }
    if pressed {
        return RefreshState::Checking;
    }
    match s.refresh_at {
        Some(t) if t > now => RefreshState::Wait(t - now),
        _ => RefreshState::Ready,
    }
}

/// The dot colour for a tray state.
pub fn state_tone(state: TrayState) -> Tone {
    match state {
        TrayState::Connected => Tone::Ok,
        TrayState::Stale => Tone::Warn,
        TrayState::Searching => Tone::Plain,
        TrayState::AuthError => Tone::Bad,
        TrayState::Attention => Tone::Accent,
    }
}

/// One headline for the link and data state.
pub fn state_title(s: &StatusSnapshot) -> &'static str {
    match (s.link.as_str(), s.tray_state()) {
        ("no_adapter", _) => "Bluetooth unavailable",
        (_, TrayState::Connected) => "Connected",
        (_, TrayState::Stale) => "Usage out of date",
        (_, TrayState::Searching) => "Looking for Clawdmeter",
        (_, TrayState::AuthError) => "Signed out of Claude Code",
        (_, TrayState::Attention) => "Claude needs you",
    }
}

/// The device's reset line (`format_reset_time` in firmware/src/ui.cpp).
pub fn reset_text(resets_at: Option<i64>, now: i64, none: &str) -> String {
    let Some(t) = resets_at else { return none.to_owned() };
    match (t - now).max(0) / 60 {
        m if m < 60 => format!("Resets in {m}m"),
        m if m < 1440 => format!("Resets in {}h {}m", m / 60, m % 60),
        m => format!("Resets in {}d {}h", m / 1440, (m % 1440) / 60),
    }
}

/// The device's bar fill for a percentage (`pct_color` in ui.cpp): green
/// below 50, amber below 80, red from 80; the light theme's own tones.
fn fill(t: &Theme, pct: u32) -> Rgba {
    match (t.dark, usage_tone(pct)) {
        (true, Tone::Ok) => rgb(0x788c5d),
        (true, Tone::Accent) => rgb(0xd97757),
        (true, _) => rgb(0xc0392b),
        (false, tone) => tone_color(t, tone),
    }
}

/// One usage panel as the device draws it: the big percentage at the top
/// left, the label as a pill at the top right, the track and fill, and the
/// reset line.
fn meter(t: &Theme, label: &'static str, pct: Option<u32>, reset: String) -> impl IntoElement {
    let value = pct.map_or_else(|| "--%".to_owned(), |p| format!("{p}%"));
    let frac = pct.unwrap_or(0).min(100) as f32 / 100.;
    div()
        .flex()
        .flex_col()
        .p(px(16.))
        .rounded(px(R_M))
        .bg(t.panel)
        .font_family(FONT_BODY)
        .child(
            div()
                .flex()
                .items_start()
                .justify_between()
                .child(div().text_size(px(36.)).line_height(px(40.)).text_color(t.text).child(value))
                .child(
                    div()
                        .mt(px(2.))
                        .px(px(12.))
                        .py(px(3.))
                        .rounded(px(R_PILL))
                        .bg(t.track)
                        .text_size(px(14.))
                        .text_color(t.text)
                        .child(label),
                ),
        )
        .child(
            div()
                .mt(px(10.))
                .h(px(14.))
                .w_full()
                .rounded(px(4.))
                .bg(t.track)
                .child(div().h_full().w(relative(frac)).rounded(px(4.)).bg(fill(t, pct.unwrap_or(0)))),
        )
        .child(div().mt(px(8.)).text_size(px(15.)).text_color(t.dim).child(reset))
}

impl Render for OverviewView {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let t = Theme::for_appearance(window.appearance());
        let now = clawdmeter_core::unix_now();
        let home = self.paths.home.clone();
        let (tone, title, sub) = match &self.snap {
            None => (Tone::Plain, "Not running", not_running_text().to_string()),
            Some(s) => {
                let sub = match (&s.account, s.age(now)) {
                    _ if s.logged_out => "Open Claude Code in a terminal and sign in.".into(),
                    (Some(a), Some(age)) => format!("{} · updated {}", tilde(a, &home), ago(age)),
                    (Some(a), None) => tilde(a, &home),
                    (None, _) => "No usage data yet".into(),
                };
                (state_tone(s.tray_state()), state_title(s), sub)
            }
        };
        let refresh = self.snap.as_ref().map(|s| self.refresh_control(&t, s, now, cx));
        let header = div()
            .flex()
            .items_start()
            .gap(px(20.))
            .child(clawd(4.))
            .child(
                div()
                    .flex()
                    .flex_col()
                    .flex_1()
                    .min_w_0()
                    .gap(px(6.))
                    .child(
                        div()
                            .flex()
                            .items_center()
                            .gap(px(10.))
                            .child(dot(&t, tone))
                            .child(div().font_family(FONT_DISPLAY).text_size(px(24.)).child(title)),
                    )
                    .child(div().text_size(px(13.)).text_color(t.mute).truncate().child(sub)),
            )
            .children(refresh);
        // The setup guide link sits at the foot of the page, bottom right.
        let text = t.text;
        let setup = div().flex_1().flex().items_end().justify_end().pt(px(16.)).child(
            div()
                .id("run-setup")
                .debug_selector(|| "run-setup".into())
                .text_size(px(13.))
                .text_color(t.dim)
                .cursor_pointer()
                .hover(move |s| s.text_color(text))
                .on_click(cx.listener(|_, _, _, cx| cx.emit(Navigate(Page::Setup))))
                .child("Run setup guide again"),
        );
        let on = self.desktop_chime;
        let chime = field(
            &t,
            "Play a chime on this computer when Claude needs you",
            "Follows the alert types on the Hooks page. Mute from the tray until restart.",
            toggle(
                &t,
                "desktop-chime",
                on,
                true,
                cx.listener(move |v, _, _, cx| {
                    match super::settings::write_settings(&v.paths, |s, _| s.desktop_chime = !on) {
                        Ok(()) => v.desktop_chime = !on,
                        Err(e) => log::warn!("couldn't save desktop_chime: {e}"),
                    }
                    cx.notify();
                }),
            ),
        );
        let mut page = div().flex_1().flex().flex_col().gap(px(20.)).child(header);
        let Some(s) = self.snap.clone() else {
            return page.child(chime).child(setup);
        };

        // No 5-hour limit (verified Ultra): the week is the only meter.
        let session = (!s.no_session_limit)
            .then(|| meter(&t, "Session", s.session_pct, reset_text(s.session_resets_at, now, "Not started")));
        let weekly = meter(&t, "Weekly", s.week_pct, reset_text(s.week_resets_at, now, "---"));
        page = page.child(div().flex().flex_col().gap(px(12.)).children(session).child(weekly));
        if let Some(tag) = self.firmware_nudge() {
            let v = tag.trim_start_matches('v').to_owned();
            page = page.child(
                notice(&t, Tone::Accent, format!("Firmware v{v} is available. Connect USB to update.")).child(
                    div()
                        .flex()
                        .gap(px(10.))
                        .pl(px(18.))
                        .child(button(
                            &t,
                            "go-update",
                            "Update firmware",
                            ButtonKind::Primary,
                            true,
                            cx.listener(|_, _, _, cx| cx.emit(Navigate(Page::Flash))),
                        ))
                        .child(button(
                            &t,
                            "skip-firmware",
                            "Skip this update",
                            ButtonKind::Normal,
                            true,
                            cx.listener(move |v, _, _, cx| v.skip_firmware(tag.clone(), cx)),
                        )),
                ),
            );
            if self.skip_failed {
                page = page.child(notice(&t, Tone::Bad, "Couldn't save that to the config file."));
            }
        }
        page.child(chime).child(setup)
    }
}

impl OverviewView {
    /// The refresh icon at the top right, with a quiet note when a press
    /// can't check right away.
    fn refresh_control(&self, t: &Theme, s: &StatusSnapshot, now: i64, cx: &mut Context<Self>) -> impl IntoElement {
        let state = refresh_state(s, self.refreshing.is_some(), now);
        let note: Option<SharedString> = match &state {
            RefreshState::Checking => Some("Checking…".into()),
            RefreshState::Wait(secs) => Some(format!("Can check again in {}", wait_text(*secs)).into()),
            RefreshState::Paused(until) => Some(
                clawdmeter_core::usage::paused_message(*until, clawdmeter_core::platform::utc_offset_secs())
                    .trim_end_matches('.')
                    .to_owned()
                    .into(),
            ),
            RefreshState::Ready => None,
        };
        let checking = state == RefreshState::Checking;
        let button =
            icon_button(t, "refresh", "refresh-cw", "Refresh usage", state == RefreshState::Ready, checking, {
                cx.listener(|v, _, _, cx| {
                    command(&v.paths, AppCommand::Refresh);
                    v.refreshing = Some(clawdmeter_core::unix_now());
                    cx.notify();
                })
            });
        div()
            .flex()
            .flex_shrink_0()
            .items_center()
            .gap(px(8.))
            .when_some(note, |d, n| d.child(div().text_size(px(12.5)).text_color(t.mute).child(n)))
            .child(button)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn refresh_follows_the_gap_and_backoff() {
        let s = StatusSnapshot { refresh_at: Some(1_100), ..Default::default() };
        assert_eq!(refresh_state(&s, false, 1_000), RefreshState::Wait(100));
        assert_eq!(refresh_state(&s, false, 1_100), RefreshState::Ready);
        assert_eq!(refresh_state(&s, true, 1_000), RefreshState::Checking);
        let polling = StatusSnapshot { polling: true, ..Default::default() };
        assert_eq!(refresh_state(&polling, false, 1_000), RefreshState::Checking);
        assert_eq!(refresh_state(&StatusSnapshot::default(), false, 0), RefreshState::Ready);
        let paused = StatusSnapshot { paused_until: Some(4_600), refresh_at: Some(4_600), ..Default::default() };
        assert_eq!(refresh_state(&paused, true, 1_000), RefreshState::Paused(4_600), "a press doesn't ask");
        assert_eq!(refresh_state(&paused, false, 4_600), RefreshState::Ready);
    }

    #[test]
    fn reset_lines_match_the_device() {
        assert_eq!(reset_text(None, 0, "Not started"), "Not started");
        assert_eq!(reset_text(Some(59 * 60 + 30), 0, "---"), "Resets in 59m");
        assert_eq!(reset_text(Some(177 * 60), 0, "---"), "Resets in 2h 57m");
        assert_eq!(reset_text(Some(5 * 1440 * 60 + 11 * 3600), 0, "---"), "Resets in 5d 11h");
    }

    #[test]
    fn waits_read_naturally() {
        assert_eq!(wait_text(0), "1 s");
        assert_eq!(wait_text(45), "45 s");
        assert_eq!(wait_text(100), "1 min 40 s");
        assert_eq!(wait_text(600), "10 min");
        assert_eq!(wait_text(3_900), "1 h 5 min");
    }
}
