//! The tray process: the daemon, the tray icon and menu, the daily
//! firmware update check, and the app window process it launches.
//!
//! The tray icon needs a platform event loop on the main thread (a Win32
//! message loop, NSApplication on macOS); on Linux it's a StatusNotifierItem
//! served over D-Bus by `ksni` on its own thread, so no GTK is linked. See
//! [`event_loop`]. When a Linux desktop has no StatusNotifierWatcher (GNOME
//! without the AppIndicator extension), the app window stands in for the
//! tray, and closing it quits the app.

mod event_loop;
#[cfg(any(target_os = "macos", test))]
pub mod header;
#[cfg(target_os = "macos")]
pub mod mac_menu;
pub mod pace;
#[cfg(target_os = "linux")]
mod sni;

use std::process::{Child, Command as Proc};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use clawdmeter_core::{unix_now, Paths, Settings};
#[cfg(not(target_os = "macos"))]
use tray_icon::menu::{CheckMenuItem, MenuItem, PredefinedMenuItem};
use tray_icon::menu::{Menu, MenuEvent};
use tray_icon::{Icon, TrayIcon, TrayIconBuilder};

use crate::art::{tray_icon_rgba, TrayState};
use crate::daemon::{self, Command, DaemonHandle};
use crate::ipc::{self, AppCommand, StatusSnapshot};
use crate::ui::overview::{refresh_settled, refresh_state};
use crate::Page;

/// Status file refresh even when nothing changed (windows use it as a
/// liveness signal).
const STATUS_REFRESH_SECS: i64 = 5;
/// Menu text refresh ("updated N s ago").
const MENU_REFRESH_SECS: i64 = 2;
/// How often the update checker wakes to see whether a check is due.
const UPDATE_POLL: Duration = Duration::from_secs(600);

/// Set by SIGINT/SIGTERM.
static QUIT: AtomicBool = AtomicBool::new(false);

/// Does this desktop show tray icons? (Always true off Linux.)
/// `CLAWDMETER_NO_TRAY=1` forces the window fallback.
pub fn tray_supported() -> bool {
    if std::env::var_os("CLAWDMETER_NO_TRAY").is_some_and(|v| v != "0") {
        return false;
    }
    #[cfg(target_os = "linux")]
    {
        sni::watcher_present()
    }
    #[cfg(not(target_os = "linux"))]
    {
        true
    }
}

/// Launch the window process (`clawdmeter --window <page>`). If one is
/// already running, the new process hands it the page and exits.
pub fn spawn_window(page: Page) -> std::io::Result<Child> {
    let exe = std::env::current_exe()?;
    Proc::new(exe).arg("--window").arg(page.as_str()).spawn()
}

/// Which of the Linux/Windows menu's optional items it has. A change
/// rebuilds the menu (`TrayIcon::set_menu`) rather than editing it in place,
/// so no item ever moves under an open menu. macOS has no native items: its
/// panel shows and hides its own parts.
#[cfg(any(not(target_os = "macos"), test))]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
struct MenuShape {
    /// The Session row (or Spend, on a spend-capped account): gone with no
    /// 5-hour limit.
    session: bool,
    /// The Weekly row: gone on a spend-capped account, which has no week.
    weekly: bool,
    /// A Codex account's reset credits ([`resets_row`]), while their count
    /// is known.
    resets: bool,
    /// "Update Clawdmeter / firmware to v…": while there's one to offer
    /// ([`update_offer`]). Flashing by hand otherwise is rare, and the
    /// window's Flash page has it.
    update: bool,
}

#[cfg(any(not(target_os = "macos"), test))]
impl MenuShape {
    fn of(snap: &StatusSnapshot, offer: Option<&Update>) -> Self {
        MenuShape {
            session: snap.spend || !snap.no_session_limit,
            weekly: !snap.spend,
            resets: snap.shown_resets().is_some(),
            update: offer.is_some(),
        }
    }
}

/// A newer release the tray offers. Every release tag ships the app and the
/// firmware together, so when both are behind the app goes first: the newer
/// app's flasher and protocol match the new firmware, and once it's
/// installed the offer moves on to the firmware.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Update {
    /// This app is older than the release (`v1.9.0`): the Update page.
    App(String),
    /// The app is current, or not a release build, and the board runs older
    /// firmware (`v1.9.0`): the Flash page.
    Firmware(String),
}

impl Update {
    /// The icon's tooltip and the item's text: "Update Clawdmeter to v1.9.0".
    pub fn label(&self) -> String {
        match self {
            Update::App(v) => format!("Update Clawdmeter to {v}"),
            Update::Firmware(v) => format!("Update firmware to {v}"),
        }
    }

    /// The window page that installs it.
    fn page(&self) -> Page {
        match self {
            Update::App(_) => Page::Update,
            Update::Firmware(_) => Page::Flash,
        }
    }
}

/// What a click on a Linux/Windows menu item asks for. (The macOS panel's
/// buttons arrive as `mac_menu::Action`s.)
#[cfg(not(target_os = "macos"))]
#[derive(Debug, Clone, PartialEq, Eq)]
enum Click {
    /// Something the window could ask for too.
    App(AppCommand),
    /// Switch the chime on this computer (`desktop_chime`), as the
    /// Overview's switch does.
    Chime,
}

/// What the tray offers, if anything. While a release build is behind the
/// latest release, the app update when the sidebar would announce it (update
/// checks on, the release not skipped; `UpdateView::announced`), and nothing
/// otherwise: the board's firmware waits for the app. Once the app is current
/// (or in a dev build, which is never offered one), the firmware update
/// (`StatusSnapshot::update_available`).
fn update_offer(snap: &StatusSnapshot, settings: &Settings, release_build: bool, app_version: &str) -> Option<Update> {
    if let Some(tag) = snap.app_update(app_version).filter(|_| release_build) {
        let skipped = settings.skipped_update.as_deref().and_then(clawdmeter_flash::release::release_tag);
        return (settings.update_check && skipped.as_ref() != Some(&tag)).then_some(Update::App(tag));
    }
    snap.update_available().map(|v| Update::Firmware(display_version(v)))
}

/// The macOS menu: the drawn panel alone (`mac_menu`), whose footer holds
/// the actions.
#[cfg(target_os = "macos")]
struct Items {
    menu: Menu,
    header: std::cell::RefCell<mac_menu::MenuHeader>,
}

#[cfg(target_os = "macos")]
impl Items {
    fn new() -> Self {
        let menu = Menu::new();
        let header = std::cell::RefCell::new(mac_menu::MenuHeader::install(&menu));
        Items { menu, header }
    }

    /// `pressed`: a refresh asked for and not yet served (the panel shows it
    /// as checking); `offer`: the update to offer ([`update_offer`]).
    fn update(
        &self,
        snap: &StatusSnapshot,
        paths: &Paths,
        settings: &Settings,
        pressed: bool,
        offer: Option<&Update>,
        now: i64,
    ) {
        let refresh = refresh_state(snap, pressed, now);
        let mut h = header::Header::new(snap, &paths.home, settings.pace_marker, settings.desktop_chime, refresh, now);
        h.update = offer.cloned();
        self.header.borrow_mut().update(&h);
    }
}

/// The Linux/Windows menu: native items. Their ids are fixed strings, so a
/// click still matches after the menu is rebuilt.
#[cfg(not(target_os = "macos"))]
struct Items {
    menu: Menu,
    /// The optional items it was built with.
    shape: MenuShape,
    status: MenuItem,
    account: MenuItem,
    session: MenuItem,
    weekly: MenuItem,
    resets: MenuItem,
    refresh: MenuItem,
    /// The chime on this computer (`desktop_chime`): the Overview's switch.
    chime: CheckMenuItem,
    update: MenuItem,
}

#[cfg(not(target_os = "macos"))]
impl Items {
    fn new(shape: MenuShape) -> Self {
        let it = Items {
            menu: Menu::new(),
            shape,
            // The status and usage rows are enabled so they read at full
            // contrast (a disabled item is greyed out); clicking one opens
            // the page with the details.
            status: MenuItem::with_id("status", "Clawdmeter · Starting", true, None),
            account: MenuItem::with_id("account", "No usage data yet", true, None),
            session: MenuItem::with_id("session", "Session  –", true, None),
            weekly: MenuItem::with_id("weekly", "Weekly  –", true, None),
            resets: MenuItem::with_id("resets", "Resets  –", true, None),
            refresh: MenuItem::with_id("refresh", "Refresh usage", true, None),
            chime: CheckMenuItem::with_id("chime", "Chime on this computer", true, false, None),
            update: MenuItem::with_id("update", "Update firmware…", true, None),
        };
        let open = MenuItem::with_id("open", "Open Clawdmeter", true, None);
        let settings = MenuItem::with_id("settings", "Settings…", true, None);
        let quit = MenuItem::with_id("quit", "Quit Clawdmeter", true, None);
        let separator = PredefinedMenuItem::separator;
        let (sep1, sep2, sep3, sep4) = (separator(), separator(), separator(), separator());
        let mut list: Vec<&dyn tray_icon::menu::IsMenuItem> = vec![&it.status, &it.account, &sep1];
        if shape.session {
            list.push(&it.session);
        }
        if shape.weekly {
            list.push(&it.weekly);
        }
        if shape.resets {
            list.push(&it.resets);
        }
        list.extend([&sep2 as &dyn tray_icon::menu::IsMenuItem, &it.refresh, &it.chime, &sep3, &open, &settings]);
        if shape.update {
            list.push(&it.update);
        }
        list.extend([&sep4 as &dyn tray_icon::menu::IsMenuItem, &quit]);
        if let Err(e) = it.menu.append_items(&list) {
            log::warn!("tray menu: {e}");
        }
        it
    }

    /// `pressed`: a refresh asked for and not yet served (the item says it's
    /// checking); `offer`: the update to offer ([`update_offer`]).
    fn update(
        &self,
        snap: &StatusSnapshot,
        paths: &Paths,
        settings: &Settings,
        pressed: bool,
        offer: Option<&Update>,
        now: i64,
    ) {
        let state = snap.tray_state();
        let mut label = state.label().to_owned();
        if snap.link == "no_adapter" {
            label = "Bluetooth unavailable".into();
        }
        self.status.set_text(format!("Clawdmeter · {label}"));
        let account = snap.account.as_deref().map(|a| crate::tilde(a, &paths.home));
        self.account.set_text(match (account, snap.age(now)) {
            (Some(a), Some(age)) => format!("{a} · updated {}", crate::ago(age)),
            (Some(a), None) => a,
            (None, _) if snap.logged_out => "Open Claude Code to sign in".into(),
            (None, _) => "No usage data yet".into(),
        });
        // The pace marker's verdict, when it's on (never for spend: its
        // period already is one, as on the device).
        let pace = |pct: Option<u32>, resets_at, window| {
            pct.filter(|_| settings.pace_marker)
                .and_then(|p| pace::pace(p, resets_at, window, now))
                .map(|(_, level)| level)
        };
        // A Codex account's main meter is "Codex"; its pace verdict shows as
        // the device draws the marker on Codex bars too.
        let (session, weekly) = meter_labels(snap);
        self.session.set_text(if snap.spend {
            meter_row("Spend", snap.session_pct, true, snap.spend_resets_at, None, now)
        } else {
            let pace = pace(snap.session_pct, snap.session_resets_at, pace::SESSION_WINDOW_SECS);
            meter_row(session, snap.session_pct, snap.window_started, snap.session_resets_at, pace, now)
        });
        let pace = pace(snap.week_pct, snap.week_resets_at, pace::WEEK_WINDOW_SECS);
        self.weekly.set_text(meter_row(weekly, snap.week_pct, true, snap.week_resets_at, pace, now));
        if let Some(held) = snap.shown_resets() {
            self.resets.set_text(resets_row(held, &snap.resets_expiries, &chrono::Local));
        }
        let (refresh, can_refresh) = refresh_item(snap, pressed, now);
        self.refresh.set_text(refresh);
        self.refresh.set_enabled(can_refresh);
        self.chime.set_checked(settings.desktop_chime);
        if let Some(u) = offer {
            self.update.set_text(format!("{}…", u.label()));
        }
    }

    /// What a click on item `id` asks for.
    fn command(id: &tray_icon::menu::MenuId, update_page: Page) -> Option<Click> {
        let open = |p: Page| Click::App(AppCommand::Open(p.as_str().into()));
        Some(match id.0.as_str() {
            "status" | "session" | "weekly" | "resets" | "open" => open(Page::Overview),
            "account" => open(Page::Accounts),
            "settings" => open(Page::Settings),
            "refresh" => Click::App(AppCommand::Refresh),
            "chime" => Click::Chime,
            "update" => open(update_page),
            "quit" => Click::App(AppCommand::Quit),
            _ => return None,
        })
    }
}

/// One usage row, with the device's reset countdown and, with the pace
/// marker on, its verdict: "Weekly  57% · resets in 3d 4h · on pace". `–`
/// with no reading (no data is never a zero).
#[cfg(any(not(target_os = "macos"), test))]
fn meter_row(
    label: &str,
    pct: Option<u32>,
    started: bool,
    resets_at: Option<i64>,
    pace: Option<pace::Level>,
    now: i64,
) -> String {
    let mut row = match (pct, resets_at) {
        (None, _) => return format!("{label}  –"),
        (Some(_), _) if !started => return format!("{label}  not started"),
        (Some(p), Some(t)) => format!("{label}  {p}% · resets in {}", crate::countdown(t - now)),
        (Some(p), None) => format!("{label}  {p}%"),
    };
    if let Some(level) = pace {
        row.push_str(" · ");
        row.push_str(level.words());
    }
    row
}

/// The refresh item's text and whether it can be clicked (Linux, Windows),
/// by the Overview's rule: only when a press would check right away. A press
/// shows as checking until that check has run (`pressed`).
#[cfg(any(not(target_os = "macos"), test))]
fn refresh_item(snap: &StatusSnapshot, pressed: bool, now: i64) -> (String, bool) {
    use crate::ui::overview::{wait_text, RefreshState};
    match refresh_state(snap, pressed, now) {
        RefreshState::Checking => ("Checking usage…".into(), false),
        RefreshState::Paused(until) => {
            let msg = clawdmeter_core::usage::paused_message_by(
                provider(snap),
                until,
                clawdmeter_core::platform::utc_offset_secs(),
            );
            (msg.trim_end_matches('.').to_owned(), false)
        }
        RefreshState::Wait(secs) => (format!("Can check again in {}", wait_text(secs)), false),
        RefreshState::Ready => ("Refresh usage".into(), true),
    }
}

/// A Codex account's reset credits in one row: "2 resets · next expires
/// Dec 1" (the soonest held one's local date, as the device shows `rx`),
/// "2 resets" with no dates known, "No resets available" with none.
#[cfg(any(not(target_os = "macos"), test))]
fn resets_row<Tz: chrono::TimeZone>(held: u32, expiries: &[i64], tz: &Tz) -> String
where
    Tz::Offset: std::fmt::Display,
{
    use chrono::Datelike;
    if held == 0 {
        return "No resets available".into();
    }
    let mut row = format!("{held} reset{}", if held == 1 { "" } else { "s" });
    if let Some(t) = expiries.first().and_then(|t| chrono::DateTime::from_timestamp(*t, 0)) {
        let l = t.with_timezone(tz);
        row.push_str(&format!(" · next expires {} {}", l.format("%b"), l.day()));
    }
    row
}

/// Whose endpoint the shown account's numbers come from.
fn provider(snap: &StatusSnapshot) -> clawdmeter_core::usage::Provider {
    if snap.codex {
        clawdmeter_core::usage::Provider::Codex
    } else {
        clawdmeter_core::usage::Provider::Claude
    }
}

/// The text beside the menu bar icon (macOS, `menu_bar_usage` on): the
/// higher of the session's (spend's, on a spend-capped account) and the
/// week's percentages, so the limit closest to stopping you. The menu shows
/// which one it is. Nothing with no reading.
fn menu_bar_title(snap: &StatusSnapshot) -> Option<String> {
    if snap.logged_out {
        return None;
    }
    snap.session_pct.max(snap.week_pct).map(|p| format!("{p}%"))
}

fn display_version(v: &str) -> String {
    if v.starts_with('v') {
        v.to_owned()
    } else {
        format!("v{v}")
    }
}

fn icon_for(state: TrayState) -> Option<Icon> {
    Icon::from_rgba(tray_icon_rgba(state, 32), 32, 32).ok()
}

/// The session and weekly meters' names. A Codex account's main meter (its
/// 5-hour window, else its week) is "Codex", as the device labels it.
fn meter_labels(snap: &StatusSnapshot) -> (&'static str, &'static str) {
    match (snap.codex, snap.no_session_limit) {
        (true, true) => ("Session", "Codex"),
        (true, false) => ("Codex", "Weekly"),
        (false, _) => ("Session", "Weekly"),
    }
}

fn tooltip(snap: &StatusSnapshot) -> String {
    let mut t = format!("Clawdmeter · {}", snap.tray_state().label());
    let (session, weekly) = meter_labels(snap);
    match (snap.session_pct, snap.week_pct) {
        (Some(s), _) if snap.spend => t.push_str(&format!(" · Spend {s}%")),
        (Some(s), Some(w)) => t.push_str(&format!(" · {session} {s}% · {weekly} {w}%")),
        (None, Some(w)) if snap.no_session_limit => t.push_str(&format!(" · {weekly} {w}%")),
        _ => {}
    }
    t
}

/// Everything the main-thread tick owns.
struct TrayApp {
    paths: Paths,
    daemon: Option<DaemonHandle>,
    items: Option<Items>,
    tray: Option<TrayIcon>,
    use_tray: bool,
    shown_state: Option<TrayState>,
    /// The text beside the menu bar icon now (macOS).
    shown_title: Option<String>,
    /// The update the menu offers now ([`update_offer`]).
    offer: Option<Update>,
    last_written: Option<StatusSnapshot>,
    last_written_at: i64,
    menu_at: i64,
    /// When a refresh was asked for, until that check has run.
    refresh_pressed: Option<i64>,
    /// The window process, while it runs.
    window: Option<Child>,
    latest: Arc<Mutex<Option<String>>>,
    check_now: Arc<AtomicBool>,
    settings: Settings,
    settings_at: i64,
}

impl TrayApp {
    /// Show the window on `page`: switch the open one, or start it.
    fn open(&mut self, page: Page) {
        if let Some(c) = self.window.as_mut() {
            if matches!(c.try_wait(), Ok(None)) {
                if let Err(e) = ipc::send_window_command(&self.paths, page) {
                    log::warn!("couldn't switch the window to {}: {e}", page.as_str());
                }
                return;
            }
        }
        match spawn_window(page) {
            Ok(c) => self.window = Some(c),
            Err(e) => log::error!("couldn't open the window: {e}"),
        }
    }

    /// Switch the chime on this computer (`desktop_chime`), the Overview's
    /// switch: saved, so it holds after a restart and the two agree. It flips
    /// what the file says, not the copy re-read every 5 s, so a change made
    /// elsewhere a moment ago isn't undone.
    fn toggle_desktop_chime(&mut self) {
        let mut on = self.settings.desktop_chime;
        let saved = crate::ui::settings::write_settings(&self.paths, |s, _| {
            s.desktop_chime = !s.desktop_chime;
            on = s.desktop_chime;
        });
        match saved {
            Ok(()) => self.settings.desktop_chime = on,
            Err(e) => log::warn!("couldn't save desktop_chime: {e}"),
        }
        // Show the new state at once.
        self.menu_at = 0;
    }

    /// Where the update icon or item leads: the Update page for the app,
    /// the Flash page for the firmware.
    fn offer_page(&self) -> Page {
        self.offer.as_ref().map_or(Page::Flash, Update::page)
    }

    fn send(&self, cmd: Command) {
        if let Some(d) = &self.daemon {
            let _ = d.commands.send(cmd);
        }
    }

    fn handle(&mut self, cmd: AppCommand) -> bool {
        match cmd {
            AppCommand::Refresh => {
                self.send(Command::RefreshNow);
                self.refresh_pressed = Some(unix_now());
                // Show the spinner now, not at the next menu refresh.
                self.menu_at = 0;
            }
            AppCommand::Reconnect => self.send(Command::Reconnect),
            AppCommand::Forget => self.send(Command::Forget),
            AppCommand::Open(w) => {
                if let Some(p) = Page::parse(&w) {
                    self.open(p);
                }
            }
            AppCommand::CheckUpdate => self.check_now.store(true, Ordering::SeqCst),
            AppCommand::SettingsChanged => {
                // Re-read on the next tick too (the menu bar text).
                self.settings_at = 0;
                self.send(Command::ReloadSettings);
            }
            AppCommand::Quit => return false,
        }
        true
    }

    fn snapshot(&self, now: i64) -> StatusSnapshot {
        let status = self.daemon.as_ref().map(|d| d.status.borrow().clone()).unwrap_or_default();
        let mut snap = StatusSnapshot::from_status(&status, now);
        snap.windowed = !self.use_tray;
        snap.latest_firmware = self.latest.lock().ok().and_then(|l| l.clone());
        snap.device_firmware = ipc::load_firmware_record(&self.paths).map(|r| r.ver);
        // Debug builds only: `CLAWDMETER_DEMO_FIRMWARE_UPDATE=1.9.0` acts as if
        // that release were out and the board older, to see the update icon
        // (the Linux/Windows item, the Overview's nudge) without one.
        #[cfg(debug_assertions)]
        if let Some(v) = std::env::var("CLAWDMETER_DEMO_FIRMWARE_UPDATE").ok().filter(|v| !v.trim().is_empty()) {
            snap.latest_firmware = Some(v.trim().to_owned());
            snap.device_firmware = Some("0.0.1".into());
        }
        // Debug builds only: `CLAWDMETER_DEMO_USAGE=1` shows made-up usage
        // (connected, Session 34 % and Weekly 62 %), the same on every OS,
        // for screenshots and for trying the menu without an account or a
        // board.
        #[cfg(debug_assertions)]
        if std::env::var("CLAWDMETER_DEMO_USAGE").is_ok_and(|v| v == "1") {
            snap = StatusSnapshot {
                link: "connected".into(),
                tray: "Connected".into(),
                account: Some(self.paths.home.join(".claude").display().to_string()),
                read_at: Some(now - 120),
                session_pct: Some(34),
                window_started: true,
                session_resets_at: Some(now + 2 * 3600 + 30 * 60),
                week_pct: Some(62),
                week_resets_at: Some(now + (5 * 24 + 11) * 3600),
                no_session_limit: false,
                spend: false,
                codex: false,
                resets_held: None,
                resets_expiries: Vec::new(),
                stale: false,
                logged_out: false,
                errors: Vec::new(),
                paused_until: None,
                polling: false,
                ..snap
            };
        }
        snap
    }

    /// One main-thread tick; false = quit.
    fn tick(&mut self) -> bool {
        if QUIT.load(Ordering::SeqCst) {
            return false;
        }
        let now = unix_now();
        if self.use_tray && self.tray.is_none() {
            self.create_tray();
        }

        // Menu clicks (Linux, Windows: macOS has no native items).
        while let Ok(ev) = MenuEvent::receiver().try_recv() {
            #[cfg(not(target_os = "macos"))]
            match Items::command(&ev.id, self.offer_page()) {
                Some(Click::App(c)) => {
                    if !self.handle(c) {
                        return false;
                    }
                }
                Some(Click::Chime) => self.toggle_desktop_chime(),
                None => {}
            }
            #[cfg(target_os = "macos")]
            let _ = ev;
        }

        // Clicks in the macOS panel.
        #[cfg(target_os = "macos")]
        for action in mac_menu::take_actions() {
            use mac_menu::Action;
            let cmd = match action {
                Action::Refresh => AppCommand::Refresh,
                Action::Open => AppCommand::Open(Page::Overview.as_str().into()),
                Action::Settings => AppCommand::Open(Page::Settings.as_str().into()),
                Action::Update => AppCommand::Open(self.offer_page().as_str().into()),
                Action::Chime => {
                    // (Two clicks in one 200 ms tick count as one.)
                    self.toggle_desktop_chime();
                    continue;
                }
                Action::Quit => AppCommand::Quit,
            };
            if !self.handle(cmd) {
                return false;
            }
            // Show what changed (the speaker, the spinner) right away.
            self.menu_at = 0;
        }

        // Commands from windows.
        for c in ipc::take_commands(&self.paths) {
            log::debug!("window command: {}", c.encode());
            if !self.handle(c) {
                return false;
            }
        }

        if now - self.settings_at >= 5 {
            self.settings_at = now;
            if let Ok(s) = Settings::load(&self.paths) {
                self.settings = s;
            }
        }

        // Status out: tray + file.
        let snap = self.snapshot(now);
        let state = snap.tray_state();
        if self.shown_state != Some(state) {
            if let Some(t) = &self.tray {
                let _ = t.set_icon(icon_for(state));
            }
            self.shown_state = Some(state);
        }
        // Only macOS draws a title beside the icon: Windows has none, and a
        // StatusNotifierItem's title is the app's name in KDE's tray settings.
        let title =
            (cfg!(target_os = "macos") && self.settings.menu_bar_usage).then(|| menu_bar_title(&snap)).flatten();
        if title != self.shown_title {
            if let Some(t) = &self.tray {
                t.set_title(title.as_deref());
            }
            self.shown_title = title;
        }
        if self.refresh_pressed.is_some_and(|at| refresh_settled(&snap, at, now)) {
            self.refresh_pressed = None;
            self.menu_at = 0;
        }
        // The menu's "updated N s ago" ticks every couple of seconds; the
        // file is rewritten on change, or every few seconds as a heartbeat.
        if now - self.menu_at >= MENU_REFRESH_SECS || self.last_written.as_ref().is_none_or(|w| !w.same_content(&snap))
        {
            self.menu_at = now;
            self.offer = update_offer(&snap, &self.settings, crate::is_release_build(), crate::version());
            // A chime switched on or off, an update found or installed, a
            // row gone or back: build the Linux/Windows menu again.
            #[cfg(not(target_os = "macos"))]
            {
                let shape = MenuShape::of(&snap, self.offer.as_ref());
                if let (Some(t), Some(items)) = (&self.tray, &self.items) {
                    if items.shape != shape {
                        let items = Items::new(shape);
                        t.set_menu(Some(Box::new(items.menu.clone())));
                        self.items = Some(items);
                    }
                }
            }
            if let Some(items) = &self.items {
                items.update(
                    &snap,
                    &self.paths,
                    &self.settings,
                    self.refresh_pressed.is_some(),
                    self.offer.as_ref(),
                    now,
                );
            }
            if let Some(t) = &self.tray {
                let _ = t.set_tooltip(Some(tooltip(&snap)));
            }
        }
        let changed = self.last_written.as_ref().is_none_or(|w| !w.same_content(&snap));
        if changed || now - self.last_written_at >= STATUS_REFRESH_SECS {
            if let Err(e) = ipc::write_status(&self.paths, &snap) {
                log::warn!("status file: {e}");
            }
            self.last_written = Some(snap);
            self.last_written_at = now;
        }

        // Reap the window; with no tray, the window is the app. A child that
        // exits because another process already has the window open (it hands
        // that one the page) doesn't end the app: it ends when no window is open.
        if self.window.as_mut().is_some_and(|c| !matches!(c.try_wait(), Ok(None))) {
            self.window = None;
        }
        self.use_tray || self.window.is_some() || ipc::window_open_elsewhere(&self.paths)
    }

    fn create_tray(&mut self) {
        // On Linux/Windows the first menu refresh rebuilds it with the real
        // shape.
        #[cfg(not(target_os = "macos"))]
        let items = Items::new(MenuShape::default());
        #[cfg(target_os = "macos")]
        let items = Items::new();
        let built = TrayIconBuilder::new()
            .with_id("clawdmeter")
            .with_menu(Box::new(items.menu.clone()))
            .with_tooltip("Clawdmeter")
            .with_icon(icon_for(TrayState::Searching).expect("icon"))
            .build();
        match built {
            Ok(t) => {
                self.tray = Some(t);
                self.items = Some(items);
                self.shown_state = Some(TrayState::Searching);
            }
            Err(e) => {
                log::error!("tray icon unavailable ({e}); showing the window instead");
                self.use_tray = false;
                self.open(Page::Overview);
            }
        }
    }

    fn shutdown(mut self) {
        if let Some(d) = self.daemon.take() {
            d.shutdown();
        }
        if let Some(mut c) = self.window.take() {
            let _ = c.kill();
        }
        let _ = std::fs::remove_file(ipc::status_file(&self.paths));
        drop(self.tray.take());
    }
}

/// Daily firmware update check on its own thread (network I/O never blocks
/// the tray). Writes the latest tag to the shared slot and the cache file.
fn spawn_update_checker(paths: Paths, latest: Arc<Mutex<Option<String>>>, now_flag: Arc<AtomicBool>) {
    let _ = std::thread::Builder::new().name("clawdmeter-update-check".into()).spawn(move || {
        let check = clawdmeter_flash::UpdateCheck::new(&paths.cache_dir);
        if let Ok(mut l) = latest.lock() {
            *l = ipc::load_latest_release(&paths);
        }
        loop {
            let settings = Settings::load(&paths).ok();
            let forced = now_flag.swap(false, Ordering::SeqCst);
            let enabled = settings.as_ref().is_none_or(|s| s.update_check);
            if forced || (enabled && check.due(unix_now())) {
                let repo =
                    settings.map(|s| s.release_repo).unwrap_or_else(|| clawdmeter_flash::DEFAULT_RELEASE_REPO.into());
                match clawdmeter_flash::release::release_for_app(&repo, crate::version()) {
                    Ok(r) => {
                        log::info!("latest firmware release: {}", r.manifest.version);
                        let _ = std::fs::create_dir_all(&paths.cache_dir);
                        let _ = std::fs::write(ipc::latest_release_file(&paths), &r.manifest.version);
                        if let Ok(mut l) = latest.lock() {
                            *l = Some(r.manifest.version);
                        }
                    }
                    Err(e) => log::info!("update check: {e} ({})", e.detail().unwrap_or_default()),
                }
                let _ = check.mark(unix_now());
            }
            // Wake early when a window asks for a check.
            for _ in 0..(UPDATE_POLL.as_secs() / 2) {
                if now_flag.load(Ordering::SeqCst) {
                    break;
                }
                std::thread::sleep(Duration::from_secs(2));
            }
        }
    });
}

fn install_signal_handler() {
    let _ = std::thread::Builder::new().name("clawdmeter-signals".into()).spawn(|| {
        let Ok(rt) = tokio::runtime::Builder::new_current_thread().enable_all().build() else { return };
        rt.block_on(async {
            #[cfg(unix)]
            {
                use tokio::signal::unix::{signal, SignalKind};
                if let Ok(mut term) = signal(SignalKind::terminate()) {
                    tokio::select! {
                        _ = tokio::signal::ctrl_c() => {}
                        _ = term.recv() => {}
                    }
                } else {
                    let _ = tokio::signal::ctrl_c().await;
                }
            }
            #[cfg(not(unix))]
            {
                let _ = tokio::signal::ctrl_c().await;
            }
        });
        QUIT.store(true, Ordering::SeqCst);
    });
}

/// Run the tray app until Quit (or the window closes, with no tray).
pub fn run(paths: Paths) -> i32 {
    let lock = match ipc::acquire_instance_lock(&paths) {
        Ok(Some(f)) => Some(f),
        Ok(None) => {
            // Already running: show its window instead of starting a second
            // daemon. (A headless `--daemon` holder ignores this.)
            log::info!("Clawdmeter is already running; opening its window");
            let _ = ipc::send_command(&paths, &AppCommand::Open(Page::Overview.as_str().into()));
            return 0;
        }
        Err(e) => {
            // Can't tell; run rather than refuse to start.
            log::warn!("instance lock unavailable ({e}); continuing");
            None
        }
    };
    run_locked(paths, lock)
}

fn run_locked(paths: Paths, _lock: Option<std::fs::File>) -> i32 {
    log::info!("=== Clawdmeter {} ===", crate::version());
    // An AppImage renews the binary copy its hooks run (see launch::hook_binary).
    if std::env::var_os("APPIMAGE").is_some() {
        let _ = crate::hooks::install::hook_binary(&paths.state_dir);
    }
    // Commands left by windows while no tray process ran (a "quit" among
    // them would end this run on its first tick).
    let stale = ipc::take_commands(&paths);
    if !stale.is_empty() {
        log::debug!("dropped {} command(s) from a previous run", stale.len());
    }
    install_signal_handler();
    let use_tray = tray_supported();
    if !use_tray {
        log::warn!(
            "no tray (CLAWDMETER_NO_TRAY is set, or no StatusNotifierWatcher on the session bus: \
             GNOME needs the AppIndicator extension); showing the window instead"
        );
    }
    let latest = Arc::new(Mutex::new(None));
    let check_now = Arc::new(AtomicBool::new(false));
    spawn_update_checker(paths.clone(), latest.clone(), check_now.clone());
    let settings = Settings::load(&paths).unwrap_or_else(|_| Settings::from_config(&Default::default(), &paths, None));
    let mut app = TrayApp {
        daemon: Some(daemon::spawn(paths.clone())),
        paths,
        items: None,
        tray: None,
        use_tray,
        shown_state: None,
        shown_title: None,
        offer: None,
        last_written: None,
        last_written_at: 0,
        menu_at: 0,
        refresh_pressed: None,
        window: None,
        latest,
        check_now,
        settings,
        settings_at: unix_now(),
    };
    // First run opens the setup wizard; without a tray the window is the app.
    if crate::onboarding::needed(&app.paths) {
        app.open(Page::Setup);
    } else if !use_tray {
        app.open(Page::Overview);
    }
    let app = std::rc::Rc::new(std::cell::RefCell::new(Some(app)));
    let tick_app = app.clone();
    event_loop::run(
        Box::new(move || tick_app.borrow_mut().as_mut().is_some_and(TrayApp::tick)),
        Box::new(move || {
            if let Some(a) = app.borrow_mut().take() {
                a.shutdown();
            }
        }),
    );
    0
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn tooltip_leaves_out_a_session_with_no_limit() {
        let snap = StatusSnapshot { session_pct: Some(5), week_pct: Some(40), ..Default::default() };
        assert!(tooltip(&snap).ends_with(" · Session 5% · Weekly 40%"));
        let ultra = StatusSnapshot { session_pct: None, no_session_limit: true, ..snap.clone() };
        assert!(tooltip(&ultra).ends_with(" · Weekly 40%"));
        assert!(!tooltip(&ultra).contains("Session"));
        let codex = StatusSnapshot { codex: true, ..ultra.clone() };
        assert!(tooltip(&codex).ends_with(" · Codex 40%"));
        let codex_5h = StatusSnapshot { codex: true, ..snap.clone() };
        assert!(tooltip(&codex_5h).ends_with(" · Codex 5% · Weekly 40%"));
    }

    #[test]
    fn meter_rows_carry_the_device_countdown() {
        let now = 1_000_000;
        let in_3d_4h = now + (3 * 24 + 4) * 3600 + 30;
        assert_eq!(meter_row("Weekly", Some(57), true, Some(in_3d_4h), None, now), "Weekly  57% · resets in 3d 4h");
        assert_eq!(
            meter_row("Session", Some(34), true, Some(now + 2 * 3600 + 56 * 60), None, now),
            "Session  34% · resets in 2h 56m"
        );
        assert_eq!(meter_row("Session", Some(34), true, None, None, now), "Session  34%");
        assert_eq!(meter_row("Session", Some(0), false, None, None, now), "Session  not started");
        assert_eq!(meter_row("Weekly", None, true, Some(in_3d_4h), None, now), "Weekly  –", "no data is never a zero");
        assert_eq!(meter_row("Session", Some(99), true, Some(now - 10), None, now), "Session  99% · resets in 0m");
        let ahead = Some(pace::Level::Ahead);
        assert_eq!(
            meter_row("Weekly", Some(57), true, Some(in_3d_4h), ahead, now),
            "Weekly  57% · resets in 3d 4h · ahead of pace"
        );
        assert_eq!(meter_row("Weekly", None, true, None, ahead, now), "Weekly  –", "no reading, no verdict");
    }

    #[test]
    fn refresh_item_follows_the_overviews_rule() {
        let now = 1_000;
        assert_eq!(refresh_item(&StatusSnapshot::default(), false, now), ("Refresh usage".into(), true));
        assert_eq!(
            refresh_item(&StatusSnapshot::default(), true, now),
            ("Checking usage…".into(), false),
            "a press shows at once"
        );
        let wait = StatusSnapshot { refresh_at: Some(now + 100), ..Default::default() };
        assert_eq!(refresh_item(&wait, false, now), ("Can check again in 1 min 40 s".into(), false));
        let polling = StatusSnapshot { polling: true, ..Default::default() };
        assert_eq!(refresh_item(&polling, false, now), ("Checking usage…".into(), false));
        let paused = StatusSnapshot { paused_until: Some(now + 600), ..Default::default() };
        let (text, enabled) = refresh_item(&paused, true, now);
        assert!(text.starts_with("Usage paused by Anthropic until ") && !text.ends_with('.'), "{text}");
        assert!(!enabled);
    }

    #[cfg(not(target_os = "macos"))]
    #[test]
    fn clicks_match_by_fixed_ids() {
        let id = |s: &str| tray_icon::menu::MenuId::new(s);
        let open = |p: Page| Some(Click::App(AppCommand::Open(p.as_str().into())));
        assert_eq!(Items::command(&id("open"), Page::Flash), open(Page::Overview));
        assert_eq!(Items::command(&id("account"), Page::Flash), open(Page::Accounts));
        assert_eq!(Items::command(&id("settings"), Page::Flash), open(Page::Settings));
        assert_eq!(Items::command(&id("update"), Page::Update), open(Page::Update));
        assert_eq!(Items::command(&id("chime"), Page::Flash), Some(Click::Chime), "the Overview's switch");
        assert_eq!(Items::command(&id("quit"), Page::Flash), Some(Click::App(AppCommand::Quit)));
        assert_eq!(Items::command(&id("12"), Page::Flash), None);
    }

    #[test]
    fn menu_bar_title_is_the_closest_limit() {
        let snap = StatusSnapshot { session_pct: Some(34), week_pct: Some(62), ..Default::default() };
        assert_eq!(menu_bar_title(&snap).as_deref(), Some("62%"));
        let busy = StatusSnapshot { session_pct: Some(80), ..snap.clone() };
        assert_eq!(menu_bar_title(&busy).as_deref(), Some("80%"));
        let idle = StatusSnapshot { session_pct: Some(0), week_pct: Some(95), ..snap.clone() };
        assert_eq!(menu_bar_title(&idle).as_deref(), Some("95%"), "no session running doesn't hide the week");
        let ultra = StatusSnapshot { session_pct: None, no_session_limit: true, ..snap.clone() };
        assert_eq!(menu_bar_title(&ultra).as_deref(), Some("62%"));
        let spend = StatusSnapshot { session_pct: Some(40), week_pct: None, ..snap.clone() };
        assert_eq!(menu_bar_title(&spend).as_deref(), Some("40%"));
        assert_eq!(menu_bar_title(&StatusSnapshot::default()), None, "no data is never a zero");
        let out = StatusSnapshot { logged_out: true, ..snap };
        assert_eq!(menu_bar_title(&out), None);
    }

    #[test]
    fn the_menu_lists_only_what_applies() {
        let quiet = Settings::from_config(&Default::default(), &Paths::under(std::path::Path::new("/home/u")), None);
        let shape = MenuShape::of(&StatusSnapshot::default(), None);
        assert!(!shape.update, "no newer release: no firmware item");
        assert!(shape.session && shape.weekly);

        let older = StatusSnapshot {
            latest_firmware: Some("2.5.0".into()),
            device_firmware: Some("2.4.0".into()),
            ..Default::default()
        };
        let offer = update_offer(&older, &quiet, false, "2.4.0");
        assert!(MenuShape::of(&older, offer.as_ref()).update);
        let ultra = StatusSnapshot { no_session_limit: true, ..Default::default() };
        assert!(!MenuShape::of(&ultra, None).session);
        let spend = StatusSnapshot { spend: true, no_session_limit: true, ..Default::default() };
        let shape = MenuShape::of(&spend, None);
        assert!(shape.session && !shape.weekly, "a Spend row and no Weekly one");
        assert!(!shape.resets, "reset credits are Codex's");
        let codex = StatusSnapshot { codex: true, no_session_limit: true, resets_held: Some(2), ..Default::default() };
        let shape = MenuShape::of(&codex, None);
        assert!(!shape.session && shape.weekly && shape.resets, "the Codex row, then its resets");
        assert!(
            !MenuShape::of(&StatusSnapshot { resets_held: None, ..codex.clone() }, None).resets,
            "count unknown: no row"
        );
        let five = StatusSnapshot { no_session_limit: false, ..codex };
        assert!(!MenuShape::of(&five, None).resets, "with a 5-hour window the week shows instead, as on the device");
    }

    #[test]
    fn a_codex_account_reads_as_codex() {
        // Weekly-only: the one row is "Codex" (the Session row isn't listed).
        let week = StatusSnapshot { codex: true, no_session_limit: true, ..Default::default() };
        assert_eq!(meter_labels(&week).1, "Codex");
        assert_eq!(meter_labels(&StatusSnapshot { no_session_limit: false, ..week.clone() }), ("Codex", "Weekly"));
        assert_eq!(meter_labels(&StatusSnapshot::default()), ("Session", "Weekly"), "Claude keeps its names");
        // A pause names OpenAI; a Claude account's still names Anthropic.
        let now = 1_000_000;
        let paused = StatusSnapshot { paused_until: Some(now + 600), refresh_at: Some(now + 600), ..week.clone() };
        assert!(refresh_item(&paused, false, now).0.starts_with("Usage paused by OpenAI until "));
        let claude = StatusSnapshot { codex: false, ..paused };
        assert!(refresh_item(&claude, false, now).0.starts_with("Usage paused by Anthropic until "));
        // The menu-bar percentage works from the week alone.
        let codex_week = StatusSnapshot { week_pct: Some(34), ..week };
        assert_eq!(menu_bar_title(&codex_week).as_deref(), Some("34%"));
    }

    #[test]
    fn resets_fit_one_row() {
        let utc = chrono::Utc;
        let dec1 = chrono::NaiveDate::from_ymd_opt(2026, 12, 1).unwrap().and_hms_opt(22, 28, 0).unwrap().and_utc();
        let dec12 = dec1 + chrono::Duration::days(11);
        assert_eq!(resets_row(2, &[dec1.timestamp(), dec12.timestamp()], &utc), "2 resets · next expires Dec 1");
        assert_eq!(resets_row(1, &[dec12.timestamp()], &utc), "1 reset · next expires Dec 12");
        assert_eq!(resets_row(3, &[], &utc), "3 resets", "no dates known");
        assert_eq!(resets_row(0, &[], &utc), "No resets available");
        // Local time picks the day, as `rx` does.
        let east = chrono::FixedOffset::east_opt(3 * 3600).unwrap();
        assert_eq!(resets_row(1, &[dec1.timestamp()], &east), "1 reset · next expires Dec 2");
    }

    #[test]
    fn the_update_offer_puts_the_app_first() {
        let settings = Settings::from_config(&Default::default(), &Paths::under(std::path::Path::new("/home/u")), None);
        let both_behind = StatusSnapshot {
            latest_firmware: Some("1.9.0".into()),
            device_firmware: Some("1.8.0".into()),
            ..Default::default()
        };
        let app = Some(Update::App("v1.9.0".into()));
        let firmware = Some(Update::Firmware("v1.9.0".into()));
        assert_eq!(update_offer(&both_behind, &settings, true, "1.8.0"), app, "the app first");
        assert_eq!(update_offer(&both_behind, &settings, true, "1.9.0"), firmware, "then the board");
        assert_eq!(update_offer(&both_behind, &settings, false, "1.8.0"), firmware, "dev builds get no app offer");
        let skipped = Settings { skipped_update: Some("1.9.0".into()), ..settings.clone() };
        assert_eq!(update_offer(&both_behind, &skipped, true, "1.8.0"), None, "the firmware waits for the app");
        let no_checks = Settings { update_check: false, ..settings.clone() };
        assert_eq!(update_offer(&both_behind, &no_checks, true, "1.8.0"), None);
        assert_eq!(update_offer(&both_behind, &no_checks, true, "1.9.0"), firmware, "as the Overview's nudge does");
        let board_current = StatusSnapshot { device_firmware: Some("1.9.0".into()), ..both_behind.clone() };
        assert_eq!(update_offer(&board_current, &settings, true, "1.8.0"), app, "the board flashed elsewhere");
        assert_eq!(update_offer(&board_current, &settings, true, "1.9.0"), None);
        assert_eq!(Update::App("v1.9.0".into()).label(), "Update Clawdmeter to v1.9.0");
        assert_eq!(Update::Firmware("v1.9.0".into()).label(), "Update firmware to v1.9.0");
    }
}
