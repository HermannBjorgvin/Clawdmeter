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
#[cfg(target_os = "linux")]
mod sni;

use std::process::{Child, Command as Proc};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use clawdmeter_core::{unix_now, Paths, Settings};
use tray_icon::menu::{CheckMenuItem, Menu, MenuEvent, MenuItem, PredefinedMenuItem};
use tray_icon::{Icon, TrayIcon, TrayIconBuilder};

use crate::art::{tray_icon_rgba, TrayState};
use crate::daemon::{self, Command, DaemonHandle};
use crate::ipc::{self, AppCommand, StatusSnapshot};
use crate::{ago, tilde, Page};

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

struct Items {
    menu: Menu,
    status: MenuItem,
    account: MenuItem,
    session: MenuItem,
    weekly: MenuItem,
    refresh: MenuItem,
    mute: CheckMenuItem,
    open: MenuItem,
    update: MenuItem,
    settings: MenuItem,
    quit: MenuItem,
}

impl Items {
    fn new() -> Self {
        let it = Items {
            menu: Menu::new(),
            status: MenuItem::new("Clawdmeter · Starting", false, None),
            account: MenuItem::new("No usage data yet", false, None),
            session: MenuItem::new("Session  –", false, None),
            weekly: MenuItem::new("Weekly  –", false, None),
            refresh: MenuItem::new("Refresh usage", true, None),
            mute: CheckMenuItem::new("Mute chimes", true, false, None),
            open: MenuItem::new("Open Clawdmeter", true, None),
            update: MenuItem::new("Flash firmware…", true, None),
            settings: MenuItem::new("Settings…", true, None),
            quit: MenuItem::new("Quit Clawdmeter", true, None),
        };
        let _ = it.menu.append_items(&[
            &it.status,
            &it.account,
            &PredefinedMenuItem::separator(),
            &it.session,
            &it.weekly,
            &PredefinedMenuItem::separator(),
            &it.refresh,
            &it.mute,
            &PredefinedMenuItem::separator(),
            &it.open,
            &it.update,
            &it.settings,
            &PredefinedMenuItem::separator(),
            &it.quit,
        ]);
        it
    }

    fn update(&self, snap: &StatusSnapshot, paths: &Paths, chimes_on: bool, now: i64) {
        let state = snap.tray_state();
        let mut label = state.label().to_owned();
        if snap.link == "no_adapter" {
            label = "Bluetooth unavailable".into();
        }
        self.status.set_text(format!("Clawdmeter · {label}"));
        let account = snap.account.as_deref().map(|a| tilde(a, &paths.home));
        self.account.set_text(match (account, snap.age(now)) {
            (Some(a), Some(age)) => format!("{a} · updated {}", ago(age)),
            (Some(a), None) => a,
            (None, _) if snap.logged_out => "Open Claude Code to sign in".into(),
            (None, _) => "No usage data yet".into(),
        });
        self.session.set_text(match snap.session_pct {
            Some(p) if snap.window_started => format!("Session  {p}%"),
            Some(_) => "Session  not started".into(),
            None => "Session  –".into(),
        });
        self.weekly.set_text(match snap.week_pct {
            Some(p) => format!("Weekly  {p}%"),
            None => "Weekly  –".into(),
        });
        self.mute.set_checked(snap.chimes_muted);
        self.mute.set_enabled(chimes_on);
        self.update.set_text(match snap.update_available() {
            Some(v) => format!("Update firmware to {}…", display_version(v)),
            None => "Flash firmware…".into(),
        });
    }
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

fn tooltip(snap: &StatusSnapshot) -> String {
    let mut t = format!("Clawdmeter · {}", snap.tray_state().label());
    if let (Some(s), Some(w)) = (snap.session_pct, snap.week_pct) {
        t.push_str(&format!(" · Session {s}% · Weekly {w}%"));
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
    last_written: Option<StatusSnapshot>,
    last_written_at: i64,
    menu_at: i64,
    chimes_muted: bool,
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

    fn send(&self, cmd: Command) {
        if let Some(d) = &self.daemon {
            let _ = d.commands.send(cmd);
        }
    }

    fn handle(&mut self, cmd: AppCommand) -> bool {
        match cmd {
            AppCommand::Refresh => self.send(Command::RefreshNow),
            AppCommand::Reconnect => self.send(Command::Reconnect),
            AppCommand::Forget => self.send(Command::Forget),
            AppCommand::Mute(m) => {
                self.chimes_muted = m;
                self.send(Command::SetChimesMuted(m));
            }
            AppCommand::Open(w) => {
                if let Some(p) = Page::parse(&w) {
                    self.open(p);
                }
            }
            AppCommand::CheckUpdate => self.check_now.store(true, Ordering::SeqCst),
            AppCommand::Quit => return false,
        }
        true
    }

    fn snapshot(&self, now: i64) -> StatusSnapshot {
        let status = self.daemon.as_ref().map(|d| d.status.borrow().clone()).unwrap_or_default();
        let mut snap = StatusSnapshot::from_status(&status, now);
        snap.chimes_muted = self.chimes_muted;
        snap.windowed = !self.use_tray;
        snap.latest_firmware = self.latest.lock().ok().and_then(|l| l.clone());
        snap.device_firmware = ipc::load_firmware_record(&self.paths).map(|r| r.ver);
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

        // Menu clicks.
        while let Ok(ev) = MenuEvent::receiver().try_recv() {
            let Some(items) = &self.items else { continue };
            let id = ev.id;
            let cmd = if id == *items.refresh.id() {
                Some(AppCommand::Refresh)
            } else if id == *items.mute.id() {
                Some(AppCommand::Mute(items.mute.is_checked()))
            } else if id == *items.open.id() {
                Some(AppCommand::Open(Page::Overview.as_str().into()))
            } else if id == *items.update.id() {
                Some(AppCommand::Open(Page::Flash.as_str().into()))
            } else if id == *items.settings.id() {
                Some(AppCommand::Open(Page::Device.as_str().into()))
            } else if id == *items.quit.id() {
                Some(AppCommand::Quit)
            } else {
                None
            };
            if let Some(c) = cmd {
                if !self.handle(c) {
                    return false;
                }
            }
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
        // The menu's "updated N s ago" ticks every couple of seconds; the
        // file is rewritten on change, or every few seconds as a heartbeat.
        if now - self.menu_at >= MENU_REFRESH_SECS || self.last_written.as_ref().is_none_or(|w| !w.same_content(&snap))
        {
            self.menu_at = now;
            if let Some(items) = &self.items {
                items.update(&snap, &self.paths, self.settings.alert_chime || self.settings.desktop_chime, now);
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
        last_written: None,
        last_written_at: 0,
        menu_at: 0,
        chimes_muted: false,
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
