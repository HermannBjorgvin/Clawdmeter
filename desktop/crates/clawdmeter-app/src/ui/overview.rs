//! Overview: what the tray menu shows, as a page. The link, the live
//! meters, a refresh button, and a nudge when new firmware is out.

use clawdmeter_core::config::ColorToken;
use clawdmeter_core::themes;
use clawdmeter_core::usage::{fable_reaches_week, FABLE_WEEKLY_SHARE_PCT};
use clawdmeter_core::{Paths, Settings};
use clawdmeter_flash::release::release_tag;
use std::path::PathBuf;

use gpui::{
    div, prelude::*, px, relative, rgb, AnyElement, Context, Corner, Entity, EventEmitter, FocusHandle, Rgba,
    SharedString, Window,
};

use super::theme::{Theme, FONT_BODY, FONT_DISPLAY, FONT_MONO, R_M, R_PILL};
use super::widgets::{
    button, clawd, dot, icon, icon_button, menu_button, menu_row, notice, one_line, popover_menu, toggle, tone_color,
    tooltip, usage_tone, ButtonKind, Dots, Tone,
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
    /// Every account ([`Settings::accounts`]): with two or more, the header
    /// has the drop-down that picks the one the device shows.
    accounts: Vec<(PathBuf, bool)>,
    /// The account chosen for the device ([`Settings::chosen_account`]);
    /// `None`: the most recent one.
    device_account: Option<PathBuf>,
    /// The account drop-down is open.
    account_menu_open: bool,
    /// The open drop-down's focus, for Escape.
    account_menu_focus: FocusHandle,
    /// The dots after "Searching" in the header.
    dots: Entity<Dots>,
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
            accounts: Vec::new(),
            device_account: None,
            account_menu_open: false,
            account_menu_focus: cx.focus_handle(),
            dots: cx.new(Dots::new),
        };
        v.reload(clawdmeter_core::unix_now());
        v
    }

    fn reload(&mut self, now: i64) {
        self.snap = live_status(&self.paths);
        if let Ok(s) = Settings::load(&self.paths) {
            self.desktop_chime = s.desktop_chime;
            self.accounts = s.accounts().map(|(d, codex)| (d.clone(), codex)).collect();
            // Nothing left to pick: the drop-down is gone, so its menu is too.
            self.account_menu_open &= self.accounts.len() > 1;
            self.device_account = s.chosen_account().cloned();
            self.skipped_firmware = s.skipped_firmware;
        }
        if let Some(at) = self.refreshing {
            if self.snap.as_ref().is_none_or(|s| refresh_settled(s, at, now)) {
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

/// A refresh pressed at `at` is over: the check ran (fresh numbers or a new
/// error) or never will. The Overview and the tray header show it as
/// checking until then.
pub fn refresh_settled(s: &StatusSnapshot, at: i64, now: i64) -> bool {
    s.read_at.is_some_and(|r| r >= at)
        || (!s.polling && now - at > REFRESH_SETTLE_SECS)
        || now - at > REFRESH_GIVE_UP_SECS
}

/// "1 min 40 s" for a wait.
pub fn wait_text(secs: i64) -> String {
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

/// The app is looking for the board (Bluetooth works): the Overview's
/// header reads "Searching" with [`Dots`] rather than [`state_title`]'s
/// longer headline, which the account drop-down would cut short.
pub fn searching(s: &StatusSnapshot) -> bool {
    s.link != "no_adapter" && s.tray_state() == TrayState::Searching
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
    match resets_at {
        Some(t) => format!("Resets in {}", crate::countdown(t - now)),
        None => none.to_owned(),
    }
}

/// The device's bar fill for a percentage (`pct_color` in ui.cpp): green
/// below 50, amber below 80, red from 80; the light theme's own tones.
fn fill(t: &Theme, pct: u32) -> Rgba {
    bar_fill(t, usage_tone(pct))
}

/// A usage bar's fill for a tone (`Ok` green, `Accent` amber, else red):
/// the device's own colours on dark, the light theme's tones on light. The
/// tray's drawn meters use it too.
pub fn bar_fill(t: &Theme, tone: Tone) -> Rgba {
    match (t.dark, tone) {
        (true, Tone::Ok) => rgb(0x788c5d),
        (true, Tone::Accent) => rgb(0xd97757),
        (true, _) => rgb(0xc0392b),
        (false, tone) => tone_color(t, tone),
    }
}

/// How much of the weekly bar the Fable overlay covers, 0..1: Fable's own %
/// times its share of the week (`fable_bar_pct` in firmware/src/fable.h), so
/// a used-up Fable limit fills half the bar. `None`: nothing to draw.
fn fable_frac(fable_pct: Option<u32>) -> Option<f32> {
    let share = fable_pct?.min(100) * FABLE_WEEKLY_SHARE_PCT;
    (share > 0).then(|| share as f32 / 10_000.)
}

/// The overlay's width (0..1 of the bar) on a weekly fill at `week_pct`, and
/// whether it reaches the fill's end. It never runs past the fill, and runs
/// to its end by the device's rule ([`fable_reaches_week`]: within 1.5
/// points of it, since Fable usage is part of the week's). The macOS tray
/// panel's Weekly meter draws the same strip (`tray::header::Meter::fable`).
pub fn fable_extent(fable_pct: Option<u32>, week_pct: u32) -> Option<(f32, bool)> {
    let f = fable_frac(fable_pct)?;
    let fable_pct = fable_pct?;
    let fill = week_pct.min(100) as f32 / 100.;
    (week_pct > 0).then(|| if fable_reaches_week(fable_pct, week_pct) { (fill, true) } else { (f.min(fill), false) })
}

/// The overlay's colour: the device's Dark or Light violet, as the meters
/// here follow the device's Dark and Light palettes (`dark`: the theme's
/// `dark`). The tray panel uses it too.
pub fn fable_color(dark: bool) -> u32 {
    let preset = if dark { themes::default_preset() } else { themes::light() };
    preset.colors.get(ColorToken::Fable)
}

/// One usage panel as the device draws it: the big percentage at the top
/// left, the label as a pill at the top right, the track and fill, and the
/// reset line. `fable` draws the Fable overlay over the fill as the device
/// does on its weekly bar: a strip shaped like the fill, cut by a box as wide
/// as the overlay, so its left end is the fill's rounded end and its right
/// end straight, or, where it reaches the fill's end, the fill's own rounded
/// corners. Shaped like the fill even where GPUI narrows the corners of a
/// short one, so it never shows outside it.
fn meter(t: &Theme, label: &'static str, pct: Option<u32>, fable: Option<u32>, reset: String) -> impl IntoElement {
    meter_in(t, label, pct, fable, reset, fill(t, pct.unwrap_or(0)))
}

/// A Codex account's meter: white / neutral, as the device marks data from
/// the GPT side, and never a Fable strip (Fable is Claude's).
fn codex_meter(t: &Theme, label: &'static str, pct: Option<u32>, reset: String) -> impl IntoElement {
    meter_in(t, label, pct, None, reset, codex_fill(t))
}

/// A Codex account's bar fill: the text colour, white on dark, as the
/// device draws it. The tray's drawn meters use it too.
pub fn codex_fill(t: &Theme) -> Rgba {
    t.text
}

fn meter_in(
    t: &Theme,
    label: &'static str,
    pct: Option<u32>,
    fable: Option<u32>,
    reset: String,
    fill: Rgba,
) -> gpui::Div {
    let value = pct.map_or_else(|| "--%".to_owned(), |p| format!("{p}%"));
    let frac = pct.unwrap_or(0).min(100) as f32 / 100.;
    let fable = fable_extent(fable, pct.unwrap_or(0)).map(|(f, _)| {
        let strip = div().h_full().w(relative(frac / f)).rounded(px(4.)).bg(rgb(fable_color(t.dark)));
        div().absolute().top_0().left_0().h_full().w(relative(f)).overflow_hidden().child(strip)
    });
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
                .relative()
                .mt(px(10.))
                .h(px(14.))
                .w_full()
                .rounded(px(4.))
                .bg(t.track)
                .child(div().h_full().w(relative(frac)).rounded(px(4.)).bg(fill))
                .children(fable),
        )
        .child(div().mt(px(8.)).text_size(px(15.)).text_color(t.dim).child(reset))
}

/// What the Resets card lists in `cells` chips: each held credit's expiry
/// date ("Nov 27", local time, as the device shows `rx`), soonest first;
/// how many held credits have no chip of their own ("+N more", in the last
/// cell); or, with none to list, the line in their place. The macOS tray
/// panel lists them the same way (`tray::header::Resets`).
#[derive(Debug, Clone, PartialEq)]
pub struct ResetChips {
    /// The dates that get a chip, soonest first.
    pub dates: Vec<String>,
    /// Held credits without a chip of their own: "+N more" when not 0.
    pub more: u32,
    /// "No resets available" or "Expiry dates unknown", with no dates.
    pub note: Option<&'static str>,
}

pub(crate) fn reset_chips<Tz: chrono::TimeZone>(held: u32, expires_at: &[i64], cells: usize, tz: &Tz) -> ResetChips
where
    Tz::Offset: std::fmt::Display,
{
    use chrono::Datelike;
    let day = |t: &i64| {
        chrono::DateTime::from_timestamp(*t, 0).map(|t| {
            let l = t.with_timezone(tz);
            format!("{} {}", l.format("%b"), l.day())
        })
    };
    let mut dates: Vec<String> = expires_at.iter().take(held as usize).filter_map(day).collect();
    if dates.len() > cells || (dates.len() == cells && (held as usize) > cells) {
        dates.truncate(cells.saturating_sub(1));
    }
    let note = match (held, dates.is_empty()) {
        (0, _) => Some("No resets available"),
        (_, true) => Some("Expiry dates unknown"),
        _ => None,
    };
    let more = if dates.is_empty() { 0 } else { held - dates.len() as u32 };
    ResetChips { dates, more, note }
}

/// The Resets card's chips: as many columns as fit the card at their
/// narrowest ("+10 more" with room), and two rows at most.
const CHIP_MIN_W: f32 = 84.;
const CHIP_GAP: f32 = 8.;
const CHIP_ROWS: usize = 2;

/// Chips per row across a card `inner_w` wide (between its padding).
fn chip_cols(inner_w: f32) -> usize {
    (((inner_w + CHIP_GAP) / (CHIP_MIN_W + CHIP_GAP)) as usize).max(3)
}

/// A Codex account's reset credits: "Resets expire" with the count in a
/// ticket pill (dim at 0), then a chip per held credit's expiry date,
/// soonest first and highlighted, in at most two rows of equal chips across
/// the card; the last cell is "+N more" when they don't all fit or a date
/// isn't known. `inner_w` is the card's width inside its padding.
fn resets_card(t: &Theme, held: u32, expires_at: &[i64], inner_w: f32) -> impl IntoElement {
    let cols = chip_cols(inner_w);
    let ResetChips { dates, more, note } = reset_chips(held, expires_at, cols * CHIP_ROWS, &chrono::Local);
    let fg = if held == 0 { t.dim } else { t.text };
    let header = div()
        .flex()
        .items_start()
        .justify_between()
        .child(div().text_size(px(22.)).line_height(px(28.)).text_color(t.text).child("Resets expire"))
        .child(
            div()
                .mt(px(2.))
                .flex()
                .items_center()
                .gap(px(6.))
                .px(px(12.))
                .py(px(3.))
                .rounded(px(R_PILL))
                .bg(t.track)
                .text_size(px(14.))
                .text_color(fg)
                .child(icon("ticket", 15., fg))
                .child(held.to_string()),
        );
    let chip = |text: String| {
        div()
            .w_full()
            .h(px(30.))
            .flex()
            .items_center()
            .justify_center()
            .rounded(px(R_PILL))
            .text_size(px(14.))
            .child(text)
    };
    let chips = dates.into_iter().enumerate().map(|(i, d)| {
        if i == 0 {
            chip(d).bg(t.text).text_color(t.panel)
        } else {
            chip(d).bg(t.track).text_color(t.text)
        }
    });
    let more = (more > 0).then(|| chip(format!("+{more} more")).bg(t.track).text_color(t.dim));
    let body = match note {
        Some(n) => div().mt(px(12.)).text_size(px(15.)).text_color(t.dim).child(n),
        None => div().mt(px(12.)).grid().grid_cols(cols as u16).gap(px(CHIP_GAP)).children(chips).children(more),
    };
    div().flex().flex_col().p(px(16.)).rounded(px(R_M)).bg(t.panel).font_family(FONT_BODY).child(header).child(body)
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
        let title = match &self.snap {
            Some(s) if searching(s) => {
                div().flex().child("Searching").child(self.dots.clone()).debug_selector(|| "searching".into())
            }
            _ => div().child(title),
        };
        let refresh = self.snap.as_ref().map(|s| self.refresh_control(&t, s, now, cx));
        let several = self.accounts.len() > 1;
        // The top right: the refresh button, its note before it. With
        // several accounts the account drop-down comes first and the note
        // goes under them both; that side gives way (the drop-down's label
        // truncates) before the title does.
        let mute = t.mute;
        let note_el = move |n: SharedString| {
            one_line(second_line(div()).debug_selector(|| "header-note".into()).text_color(mute)).child(n)
        };
        let controls = match (several, refresh) {
            // The note gives way (an ellipsis) before the title does.
            (false, refresh) => {
                div().flex().min_w_0().h(px(HEADER_ROW_H)).items_center().children(refresh.map(|(note, button)| {
                    div().flex().min_w_0().items_center().gap(px(8.)).children(note.map(note_el)).child(button)
                }))
            }
            (true, refresh) => {
                let (note, button) = refresh.map_or((None, None), |(n, b)| (n, Some(b)));
                // Rows stretched to the column's width and their contents
                // pushed right: a right-aligned row wider than the column
                // would spill to the left, over Clawd and the title.
                div()
                    .flex()
                    .flex_col()
                    .gap(px(6.))
                    .min_w_0()
                    .child(
                        div()
                            .flex()
                            .h(px(HEADER_ROW_H))
                            .items_center()
                            .justify_end()
                            .gap(px(8.))
                            .min_w_0()
                            .child(self.account_control(&t, window, cx))
                            .children(button),
                    )
                    .children(note.map(|n| note_el(n).text_right()))
            }
        };
        // The page column's width inside its padding. Short of room for the
        // title, the drop-down and the refresh button side by side, the
        // header draws Clawd smaller and closes up its gaps.
        let page_w = (f32::from(window.viewport_size().width) - super::SIDEBAR_W).min(super::PAGE_MAX_W)
            - 2. * super::PAGE_PAD_X;
        let compact = page_w < HEADER_FULL_W;
        let header = div()
            .flex()
            .items_start()
            .gap(px(if compact { 12. } else { 20. }))
            .child(clawd(if compact { 3. } else { 4. }))
            .child(
                div()
                    .flex()
                    .flex_col()
                    .flex_1()
                    .min_w_0()
                    .min_w(px(TITLE_MIN_W))
                    .gap(px(6.))
                    .child(
                        div().flex().h(px(HEADER_ROW_H)).items_center().gap(px(10.)).child(dot(&t, tone)).child(
                            one_line(title)
                                .font_family(FONT_DISPLAY)
                                .text_size(px(24.))
                                .line_height(px(HEADER_ROW_H))
                                .min_w_0()
                                .overflow_hidden(),
                        ),
                    )
                    .child(
                        one_line(second_line(div()))
                            .debug_selector(|| "header-sub".into())
                            .text_color(t.mute)
                            .child(sub),
                    ),
            )
            .child(controls);
        // The page's foot: the chime on this computer (bell and switch, the
        // heading as their tooltip) at the left, the setup guide link at the
        // right.
        let on = self.desktop_chime;
        let chime = div()
            .id("desktop-chime-tip")
            .flex()
            .items_center()
            .gap(px(8.))
            .tooltip(tooltip(CHIME_TIP))
            .child(icon("bell", 16., t.dim))
            .child(toggle(
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
            ));
        let text = t.text;
        let setup = div()
            .id("run-setup")
            .debug_selector(|| "run-setup".into())
            .text_size(px(13.))
            .text_color(t.dim)
            .cursor_pointer()
            .hover(move |s| s.text_color(text))
            .on_click(cx.listener(|_, _, _, cx| cx.emit(Navigate(Page::Setup))))
            .child("Run setup guide again");
        let foot = div()
            .flex_1()
            .flex()
            .flex_col()
            .justify_end()
            .pt(px(16.))
            .child(div().flex().items_center().justify_between().gap(px(16.)).child(chime).child(setup));
        let mut page = div().flex_1().flex().flex_col().gap(px(20.)).child(header);
        let Some(s) = self.snap.clone() else {
            return page.child(foot);
        };

        if s.codex {
            // A Codex account (never spend-capped), as the device draws it:
            // its main meter (the 5-hour window, else the week) is "Codex";
            // the second card is the week with a 5-hour window, else the
            // reset credits.
            let meters = div().flex().flex_col().gap(px(12.));
            // The cards' width: the page column beside the sidebar, inside its padding.
            let card_w = page_w;
            let session_reset = reset_text(s.session_resets_at, now, "Not started");
            let week_reset = reset_text(s.week_resets_at, now, "---");
            page = page.child(if s.no_session_limit {
                meters
                    .child(codex_meter(&t, "Codex", s.week_pct, week_reset))
                    .children(s.shown_resets().map(|h| resets_card(&t, h, &s.resets_expiries, card_w - 32.)))
            } else {
                meters
                    .child(codex_meter(&t, "Codex", s.session_pct, session_reset))
                    .child(codex_meter(&t, "Weekly", s.week_pct, week_reset))
            });
        } else {
            // A spend-capped account has one meter, the spend; no 5-hour limit
            // (verified Ultra) leaves the week alone.
            let meters: Vec<_> = if s.spend {
                vec![meter(&t, "Spend", s.session_pct, None, reset_text(s.spend_resets_at, now, "---"))]
            } else {
                let session = (!s.no_session_limit).then(|| {
                    meter(&t, "Session", s.session_pct, None, reset_text(s.session_resets_at, now, "Not started"))
                });
                session
                    .into_iter()
                    .chain([meter(&t, "Weekly", s.week_pct, s.shown_fable(), reset_text(s.week_resets_at, now, "---"))])
                    .collect()
            };
            page = page.child(div().flex().flex_col().gap(px(12.)).children(meters));
        }
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
        page.child(foot)
    }
}

impl OverviewView {
    /// The refresh icon at the top right, with a quiet note when a press
    /// can't check right away.
    /// The refresh button and the note that goes with it (checking, how long
    /// until it can check again, or the usage endpoint's pause).
    fn refresh_control(
        &self,
        t: &Theme,
        s: &StatusSnapshot,
        now: i64,
        cx: &mut Context<Self>,
    ) -> (Option<SharedString>, AnyElement) {
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
        (note, button.into_any_element())
    }

    /// The drop-down that picks the account the device shows: the most
    /// recent one (the default) or one of them (`device_account`). Its menu
    /// hangs under it, right edges lined up.
    fn account_control(&self, t: &Theme, window: &Window, cx: &mut Context<Self>) -> impl IntoElement {
        let label = match &self.device_account {
            Some(d) => tilde(&d.to_string_lossy(), &self.paths.home),
            None => MOST_RECENT.to_owned(),
        };
        let trigger = menu_button(
            t,
            "device-account",
            Some("users"),
            label,
            ButtonKind::Small,
            true,
            cx.listener(|v, _, window, cx| {
                if v.account_menu_open {
                    v.close_account_menu(cx);
                } else {
                    v.account_menu_open = true;
                    window.focus(&v.account_menu_focus);
                    cx.notify();
                }
            }),
        );
        div().flex().flex_col().min_w_0().child(trigger).when(self.account_menu_open, |d| {
            d.child(div().flex().justify_end().children(self.account_menu(t, window, cx)))
        })
    }

    /// The drop-down's choices, the chosen one ticked. Most recent names the
    /// one it shows now; Codex folders say so.
    fn account_menu(&self, t: &Theme, window: &Window, cx: &mut Context<Self>) -> [AnyElement; 2] {
        let home = &self.paths.home;
        let row = |id: String,
                   label: String,
                   mono: bool,
                   hint: String,
                   choice: Option<PathBuf>,
                   cx: &mut Context<Self>| {
            let ticked = self.device_account == choice;
            let name = div()
                .flex()
                .items_center()
                .gap(px(8.))
                .min_w_0()
                .child(div().w(px(14.)).flex_shrink_0().when(ticked, |d| d.child(icon("check", 14., t.text))))
                .child(
                    div()
                        .text_size(px(13.5))
                        .text_color(t.text)
                        .truncate()
                        .when(mono, |d| d.font_family(FONT_MONO).text_size(px(13.)))
                        .child(label),
                );
            menu_row(t, id, name, hint, t.mute, cx.listener(move |v, _, _, cx| v.choose_account(choice.clone(), cx)))
                .into_any_element()
        };
        let shown = match (&self.device_account, self.snap.as_ref().and_then(|s| s.account.as_deref())) {
            (None, Some(a)) => tilde(a, home),
            _ => String::new(),
        };
        let mut rows = vec![row("device-account-auto".into(), MOST_RECENT.into(), false, shown, None, cx)];
        for (i, (d, codex)) in self.accounts.iter().enumerate() {
            let label = tilde(&d.to_string_lossy(), home);
            let hint = if *codex { "Codex" } else { "" }.to_owned();
            rows.push(row(format!("device-account-{i}"), label, true, hint, Some(d.clone()), cx));
        }
        let me = cx.entity().downgrade();
        popover_menu(
            t,
            "device-account-menu",
            &self.account_menu_focus,
            window.viewport_size(),
            Corner::TopRight,
            rows,
            move |_, cx| {
                let _ = me.update(cx, |v, cx| v.close_account_menu(cx));
            },
        )
    }

    fn close_account_menu(&mut self, cx: &mut Context<Self>) {
        self.account_menu_open = false;
        cx.notify();
    }

    /// Save the account the device shows (`None`: the most recent one).
    /// The tray hands it to the daemon at once.
    fn choose_account(&mut self, choice: Option<PathBuf>, cx: &mut Context<Self>) {
        self.account_menu_open = false;
        let saved = choice.clone();
        match super::settings::write_settings(&self.paths, move |s, _| s.device_account = saved) {
            Ok(()) => self.device_account = choice,
            Err(e) => log::warn!("couldn't save the device account: {e}"),
        }
        cx.notify();
    }

    /// Open or close the account drop-down (tests and the demo).
    pub fn set_account_menu_open(&mut self, open: bool) {
        self.account_menu_open = open;
    }

    /// Whether the account drop-down is open (tests).
    pub fn account_menu_open(&self) -> bool {
        self.account_menu_open
    }
}

/// The drop-down's default: the account whose usage rose most recently.
pub(crate) const MOST_RECENT: &str = "Most recent";

/// The header title's room beside the controls, which give way first:
/// "Searching..." whole.
pub(crate) const TITLE_MIN_W: f32 = 160.;

/// The page width (inside its padding) below which the header goes compact
/// (Clawd at 3 px cells, 12 px gaps), so that in the narrowest window (440
/// px) the title, "Most recent" and the refresh button still fit side by
/// side: 72 + 2 × 12 + 160 + about 183.
const HEADER_FULL_W: f32 = 480.;

/// The header's first row (the title, and beside it the drop-down and the
/// refresh button), the same height on both sides so the lines under them
/// share a baseline.
const HEADER_ROW_H: f32 = 32.;

/// The header's second lines (the account and its age, the refresh note):
/// one size and line height, so they sit on one baseline.
fn second_line(d: gpui::Div) -> gpui::Div {
    d.text_size(px(13.)).line_height(px(18.))
}

/// The chime switch's tooltip: what it does.
const CHIME_TIP: &str = "Play a chime on this computer when Claude needs you";

#[cfg(test)]
mod tests {
    use super::super::{PAGE_MAX_W, PAGE_PAD_X, SIDEBAR_W};
    use super::*;

    #[test]
    fn fable_overlay_is_half_the_bar_at_the_limit() {
        assert_eq!(fable_frac(None), None);
        assert_eq!(fable_frac(Some(0)), None, "nothing to draw at 0 %");
        assert_eq!(fable_frac(Some(40)), Some(0.2));
        assert_eq!(fable_frac(Some(100)), Some(0.5));
        assert_eq!(fable_frac(Some(250)), Some(0.5), "past the limit stays at halfway");
        // Straight-edged inside the fill; clamped to it, with its corner, at its end.
        assert_eq!(fable_extent(Some(40), 60), Some((0.2, false)));
        assert_eq!(fable_extent(Some(100), 72), Some((0.5, false)));
        assert_eq!(fable_extent(Some(80), 30), Some((0.3, true)));
        assert_eq!(fable_extent(Some(40), 0), None);
        assert_eq!(fable_extent(None, 60), None);
        // Fable is part of the week: within 1.5 points of the fill's end (the
        // two rounded apart) runs to it, as on the device; a real gap stays.
        assert_eq!(fable_extent(Some(61), 31), Some((0.31, true)));
        assert_eq!(fable_extent(Some(60), 30), Some((0.30, true)));
        assert_eq!(fable_extent(Some(40), 60), Some((0.2, false)));
        for (w, wf) in [(30, 58), (40, 78), (49, 96), (7, 12), (40, 77)] {
            assert_eq!(fable_extent(Some(wf), w).map(|e| e.1), Some(true), "{w}/{wf}");
        }
        assert_eq!(fable_extent(Some(76), 40), Some((0.38, false)), "2 points short");
        assert_eq!(fable_extent(Some(70), 40), Some((0.35, false)), "other models' 5 points");
        assert_eq!(fable_extent(Some(2), 10), Some((0.01, false)), "a sliver inside a short fill");
    }

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
    fn reset_chips_list_each_date() {
        let utc = chrono::Utc;
        let day = |d: u32| {
            chrono::NaiveDate::from_ymd_opt(2026, 11, d).unwrap().and_hms_opt(12, 0, 0).unwrap().and_utc().timestamp()
        };
        let none = ResetChips { dates: vec![], more: 0, note: Some("No resets available") };
        assert_eq!(reset_chips(0, &[], 12, &utc), none);
        let undated = ResetChips { dates: vec![], more: 0, note: Some("Expiry dates unknown") };
        assert_eq!(reset_chips(2, &[], 12, &utc), undated);
        let three = reset_chips(3, &[day(19), day(20), day(27)], 12, &utc);
        assert_eq!(
            three,
            ResetChips { dates: vec!["Nov 19".into(), "Nov 20".into(), "Nov 27".into()], more: 0, note: None }
        );
        let ten: Vec<i64> = (17..27).map(day).collect();
        // Twelve cells: all ten dates.
        let c = reset_chips(10, &ten, 12, &utc);
        assert_eq!((c.dates.len(), c.more), (10, 0));
        // Eight cells: seven dates and "+3 more".
        let c = reset_chips(10, &ten, 8, &utc);
        assert_eq!((c.dates.len(), c.dates[6].as_str(), c.more), (7, "Nov 23", 3));
        // Exactly full: every date, no "+N".
        assert_eq!(reset_chips(8, &ten[..8], 8, &utc).more, 0);
        // Twelve held, ten dates sent, ten cells: nine dates and "+3 more".
        let c = reset_chips(12, &ten, 10, &utc);
        assert_eq!((c.dates.len(), c.more), (9, 3));
        // Twelve held, ten dates, twelve cells: ten dates and "+2 more".
        assert_eq!(reset_chips(12, &ten, 12, &utc).more, 2);
        // Never more dates than credits held.
        assert_eq!(reset_chips(1, &ten, 12, &utc).dates, ["Nov 17"]);
        // Local time picks the day, as `rx` does.
        let east = chrono::FixedOffset::east_opt(14 * 3600).unwrap();
        assert_eq!(reset_chips(1, &[day(30)], 12, &east).dates, ["Dec 1"]);
    }

    #[test]
    fn chips_fill_two_rows_across_the_card() {
        // The narrowest window (720 px) and the default (880 px).
        let inner = |w: f32| (w - SIDEBAR_W).min(PAGE_MAX_W) - 2. * PAGE_PAD_X - 32.;
        assert_eq!(chip_cols(inner(720.)), 4);
        assert_eq!(chip_cols(inner(880.)), 6);
        assert_eq!(chip_cols(inner(2000.)), 6);
    }

    #[test]
    fn reset_lines_match_the_device() {
        assert_eq!(reset_text(None, 0, "Not started"), "Not started");
        assert_eq!(reset_text(Some(59 * 60 + 29), 0, "---"), "Resets in 59m");
        assert_eq!(reset_text(Some(59 * 60 + 30), 0, "---"), "Resets in 1h 0m", "rounded, like the payload's minutes");
        assert_eq!(reset_text(Some(177 * 60), 0, "---"), "Resets in 2h 57m");
        assert_eq!(reset_text(Some(5 * 1440 * 60 + 11 * 3600), 0, "---"), "Resets in 5d 11h");
    }

    #[test]
    fn searching_needs_bluetooth() {
        let snap =
            |link: &str, tray: &str| StatusSnapshot { link: link.into(), tray: tray.into(), ..Default::default() };
        assert!(searching(&snap("searching", "Searching")));
        assert!(searching(&snap("connecting", "Searching")));
        assert!(!searching(&snap("no_adapter", "Searching")), "Bluetooth unavailable says so");
        assert!(!searching(&snap("connected", "Connected")));
        assert!(!searching(&snap("connected", "Stale")));
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
