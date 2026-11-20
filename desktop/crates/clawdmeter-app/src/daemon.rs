//! The daemon runtime: drives [`clawdmeter_core::Engine`] with real I/O
//! (the `claude` CLI, events.log, attention files, the BLE link).
//!
//! It runs on its own thread with a current-thread tokio runtime, so the GUI
//! event loop never shares a reactor with it. The UI talks to it through a
//! [`DaemonHandle`]: commands in, a [`Status`] snapshot out.

use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, SystemTime};

use clawdmeter_ble::{
    should_wake, AddressCache, Bluetooth, DeviceEvent, DeviceEvents, DeviceInfo, DeviceLink, LinkError, LinkOptions,
    Reconnect, Sighting, UpdateError, UpdateProgress, MAX_WRITE,
};
use clawdmeter_core::attention::AttentionInbox;
use clawdmeter_core::backoff;
use clawdmeter_core::codex::{self, CodexCache};
use clawdmeter_core::engine::PollPlan;
use clawdmeter_core::events::EventTail;
use clawdmeter_core::platform::AppearanceWatch;
use clawdmeter_core::probe;
use clawdmeter_core::usage;
use clawdmeter_core::{unix_now, Engine, EngineStatus, HostClock, Paths, Settings, WriteKind};
use serde::{Deserialize, Serialize};
use tokio::sync::{mpsc, watch};
use tokio::task::JoinHandle;

/// Loop tick: events reach the device within about this long.
const TICK: Duration = Duration::from_millis(500);
/// Config re-read interval (s).
const SETTINGS_EVERY: i64 = 5;
/// Connection check interval (s).
const LINK_CHECK_EVERY: i64 = 5;
/// How long a board that restarted into new firmware has to come back (s).
const UPDATE_RECONNECT_SECS: i64 = 120;
/// Until this long after DONE, a link to the old version may still be the
/// old firmware in the second before its restart (s).
const UPDATE_SETTLE_SECS: i64 = 20;
/// The new version counts as running once a payload was written to it and
/// the link stayed up this long after (the board keeps the image only after a
/// payload parses; one that crashes on it drops the link and goes back).
/// Longer than the 6 s supervision timeout plus a link check (s).
const UPDATE_WRITE_SETTLE_SECS: i64 = 20;
/// Or once a link to it has stayed up this long with nothing to write (s).
const UPDATE_LINK_SETTLE_SECS: i64 = 30;
/// How long a finished or failed update stays in the status (s).
const UPDATE_SHOWN_SECS: i64 = 600;

/// Requests from the UI.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Command {
    /// Poll `/usage` now.
    RefreshNow,
    /// Drop the link and look for the device again (e.g. after pairing).
    Reconnect,
    /// Forget the cached device address, then look again (after swapping boards).
    Forget,
    /// Re-read the config now (the window saved it), not at the next
    /// periodic re-read.
    ReloadSettings,
    /// Send a firmware image to the connected board over Bluetooth.
    UpdateFirmware(FirmwareRequest),
    /// Stop the update in progress.
    CancelFirmwareUpdate,
    /// Stop the daemon thread.
    Shutdown,
}

/// A firmware image the window has fetched and checked, to send over
/// Bluetooth (`<state>/firmware-update.json`, see `ipc::firmware_request_file`).
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct FirmwareRequest {
    /// The application image (not the merged one).
    pub image: PathBuf,
    /// The board it was built for.
    pub env: String,
    /// Its release version.
    pub version: String,
    /// SHA-256 of `image`, hex: the file must still be what was checked.
    pub sha256: String,
    /// Tells this request's progress apart from an earlier one's in the
    /// status (the window picks it; unix milliseconds).
    #[serde(default)]
    pub id: u64,
}

/// A firmware update over Bluetooth, as far as it has got.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum FirmwareUpdate {
    /// Asking the board to start.
    Starting {
        /// The version being installed.
        version: String,
    },
    /// The board asks on its screen; waiting for someone to allow it there.
    Confirming {
        /// The version being installed.
        version: String,
    },
    /// Writing: `done` of `total` bytes are in the board's flash.
    Sending {
        /// The version being installed.
        version: String,
        /// Bytes written.
        done: u32,
        /// Image size.
        total: u32,
    },
    /// Every byte is in; the board checks the image.
    Verifying {
        /// The version being installed.
        version: String,
    },
    /// The board accepted it and restarts; waiting for it to reconnect
    /// with the new version.
    Restarting {
        /// The version being installed.
        version: String,
        /// Unix seconds it started restarting.
        since: i64,
    },
    /// The board came back running `version`.
    Done {
        /// The version installed.
        version: String,
        /// Unix seconds.
        at: i64,
    },
    /// It stopped.
    Failed {
        /// The version that was being installed.
        version: String,
        /// What to tell the user.
        error: String,
        /// Unix seconds.
        at: i64,
    },
}

impl FirmwareUpdate {
    /// Still going (no new update may start).
    pub fn running(&self) -> bool {
        !matches!(self, FirmwareUpdate::Done { .. } | FirmwareUpdate::Failed { .. })
    }
}

/// Versions match, with or without a leading `v`.
fn same_version(a: &str, b: &str) -> bool {
    a.trim_start_matches(['v', 'V']) == b.trim_start_matches(['v', 'V'])
}

/// Where an update waiting for the board's restart stands, `now`: still
/// waiting (`None`), done, or failed. `info` is the connected board's INFO
/// (`None` while disconnected or when the read failed), `linked_at` when that
/// link came up, `wrote_at` the first payload written over it.
fn restart_check(
    version: &str,
    since: i64,
    now: i64,
    info: Option<&DeviceInfo>,
    linked_at: Option<i64>,
    wrote_at: Option<i64>,
) -> Option<FirmwareUpdate> {
    let failed = |error: String| Some(FirmwareUpdate::Failed { version: version.to_owned(), error, at: now });
    match info {
        Some(i) if same_version(&i.ver, version) => {
            let settled = wrote_at.is_some_and(|w| now - w >= UPDATE_WRITE_SETTLE_SECS)
                || linked_at.is_some_and(|l| now - l >= UPDATE_LINK_SETTLE_SECS);
            settled.then(|| FirmwareUpdate::Done { version: version.to_owned(), at: now })
        }
        Some(i) if now - since >= UPDATE_SETTLE_SECS => {
            // It went back, or the update never reached its restart: either
            // way the old firmware runs.
            failed(format!("The update didn't take: the board still runs {}.", i.ver))
        }
        Some(_) => None,
        None if now - since > UPDATE_RECONNECT_SECS => failed(if linked_at.is_some() {
            "The board reconnected but didn't say which firmware it runs. Check the Device page.".into()
        } else {
            "The board hasn't reconnected since it restarted. If it stays dark or disconnected, turn it off and \
             on: it goes back to the firmware it had."
                .into()
        }),
        None => None,
    }
}

/// An update task the daemon is running.
struct UpdateRun {
    task: JoinHandle<Result<(), UpdateError>>,
    progress: Arc<Mutex<UpdateProgress>>,
    cancel: Arc<AtomicBool>,
    version: String,
}

/// Check a request against the link and start sending it.
fn start_update(link: Option<&DeviceLink>, req: &FirmwareRequest) -> Result<UpdateRun, String> {
    let link = link.ok_or("The board isn't connected over Bluetooth.")?;
    let updater = link.updater().ok_or(match link.info() {
        Some(i) if i.ota_slot > 0 => {
            "The board's firmware uses a newer update protocol. Update Clawdmeter on this \
                                      computer first, or install over USB."
        }
        _ => "This board's firmware can't be updated over Bluetooth.",
    })?;
    if updater.info().env != req.env {
        return Err(format!("This firmware is for {}, but the connected board is {}.", req.env, updater.info().env));
    }
    let image = std::fs::read(&req.image).map_err(|e| {
        format!("Couldn't read the firmware ({}). {}", req.image.display(), crate::errors::io_message(&e))
    })?;
    if !clawdmeter_flash::release::sha256_hex(&image).eq_ignore_ascii_case(&req.sha256) {
        return Err("The firmware file changed after it was checked. Try again.".into());
    }
    let progress = Arc::new(Mutex::new(UpdateProgress::Starting));
    let cancel = Arc::new(AtomicBool::new(false));
    let (p, c, env) = (progress.clone(), cancel.clone(), req.env.clone());
    log::info!("firmware update: sending {} ({} bytes) to the board", req.version, image.len());
    let task = tokio::spawn(async move {
        updater
            .run(
                &image,
                &env,
                |u| {
                    if let Ok(mut g) = p.lock() {
                        *g = u;
                    }
                },
                &c,
            )
            .await
    });
    Ok(UpdateRun { task, progress, cancel, version: req.version.clone() })
}

/// The BLE side, for the tray's status dot.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub enum LinkState {
    /// Not tried yet.
    #[default]
    Starting,
    /// No adapter, or Bluetooth is off.
    NoAdapter(String),
    /// No paired device found; retrying.
    Searching,
    /// Connecting to a known device.
    Connecting,
    /// Connected.
    Connected {
        /// Cache key (address).
        key: String,
        /// Device name.
        name: Option<String>,
    },
}

/// Everything the tray needs, updated every tick.
#[derive(Debug, Clone, Default)]
pub struct Status {
    /// BLE link.
    pub link: LinkState,
    /// Usage / hooks / attention.
    pub engine: EngineStatus,
    /// Unix seconds of the last successful device write.
    pub last_write_at: Option<i64>,
    /// `claude` CLI path in use (None = not found).
    pub claude: Option<PathBuf>,
    /// What the connected board's INFO characteristic says (firmware
    /// before Bluetooth updates has none).
    pub device: Option<DeviceInfo>,
    /// A firmware update over Bluetooth, running or just finished.
    pub firmware_update: Option<FirmwareUpdate>,
    /// The [`FirmwareRequest::id`] it belongs to.
    pub firmware_update_id: u64,
}

/// Handle held by the UI.
#[derive(Debug)]
pub struct DaemonHandle {
    /// Send commands.
    pub commands: mpsc::UnboundedSender<Command>,
    /// Watch status.
    pub status: watch::Receiver<Status>,
    thread: Option<std::thread::JoinHandle<()>>,
}

impl DaemonHandle {
    /// Ask the daemon to stop and wait for it.
    pub fn shutdown(mut self) {
        let _ = self.commands.send(Command::Shutdown);
        if let Some(t) = self.thread.take() {
            let _ = t.join();
        }
    }
}

/// Start the daemon on a background thread.
pub fn spawn(paths: Paths) -> DaemonHandle {
    let (cmd_tx, cmd_rx) = mpsc::unbounded_channel();
    let (st_tx, st_rx) = watch::channel(Status::default());
    let thread = std::thread::Builder::new()
        .name("clawdmeter-daemon".into())
        .spawn(move || {
            let rt = tokio::runtime::Builder::new_current_thread().enable_all().build().expect("tokio runtime");
            rt.block_on(run(paths, cmd_rx, st_tx));
        })
        .expect("spawn daemon thread");
    DaemonHandle { commands: cmd_tx, status: st_rx, thread: Some(thread) }
}

/// Re-read the config into the engine, finding `claude` again when its path
/// changed (or it wasn't found). An unreadable file keeps the settings in
/// use.
fn reload_settings(paths: &Paths, engine: &mut Engine, claude: &mut Option<PathBuf>) {
    let Ok(s) = Settings::load(paths) else { return };
    if s.claude_path != engine.settings().claude_path || claude.is_none() {
        *claude = usage::find_claude(s.claude_path.as_deref(), &paths.home);
    }
    engine.update_settings(s);
    engine.set_claude_found(claude.is_some());
}

type PollResults = Vec<(PathBuf, probe::Polled)>;
/// A Codex poll's results, and the reset-credit cache it carried.
type CodexResults = (PollResults, CodexCache);
type ConnectResult = Result<(DeviceLink, DeviceEvents), LinkError>;

/// Read each account of a poll plan: the probe, `/usage`, or a reading known
/// without a call. The probe runs from `cwd` (the state dir).
fn poll_all(claude: Option<PathBuf>, plan: PollPlan, home: PathBuf, cwd: PathBuf) -> PollResults {
    let now = chrono::Utc::now();
    plan.accounts
        .into_iter()
        .zip(plan.plans)
        .map(|(d, p)| {
            let r = probe::poll(claude.as_deref(), &d, &home, &cwd, &p, now);
            (d, r)
        })
        .collect()
}

/// Read each Codex account due (blocking; the endpoint is slow).
fn poll_codex(dirs: Vec<PathBuf>, mut cache: CodexCache) -> CodexResults {
    let now = chrono::Utc::now();
    let results = dirs.into_iter().map(|d| (d.clone(), codex::poll(&d, now, &mut cache))).collect();
    (results, cache)
}

/// The daemon loop. Returns on [`Command::Shutdown`] or when every command
/// sender is dropped.
pub async fn run(paths: Paths, mut commands: mpsc::UnboundedReceiver<Command>, status: watch::Sender<Status>) {
    let settings = Settings::load(&paths).unwrap_or_else(|e| {
        log::warn!("config unreadable ({e}); using defaults");
        Settings::from_config(&Default::default(), &paths, None)
    });
    log::info!(
        "accounts: {}",
        settings.config_dirs.iter().map(|d| d.display().to_string()).collect::<Vec<_>>().join(", ")
    );
    if !settings.codex_dirs.is_empty() {
        log::info!(
            "Codex accounts (trial): {}",
            settings.codex_dirs.iter().map(|d| d.display().to_string()).collect::<Vec<_>>().join(", ")
        );
    }
    let mut engine = Engine::new(settings);
    // Blocks the usage endpoint set survive restarts (a call during one
    // lengthens it).
    engine.restore_blocks(&backoff::load_blocks(&paths.usage_blocks_file()), unix_now());
    let mut saved_blocks = engine.blocks(unix_now());
    let host = HostClock { utc_offset_secs: 0, prefers_12h: clawdmeter_core::platform::prefers_12h() };
    // System's Dark or Light (and Solarized's) follows the OS.
    // The engine has Dark until the first answer (no write goes out before a
    // link and a poll anyway).
    let appearance = AppearanceWatch::spawn();
    let mut shown_appearance = None;
    let inbox = AttentionInbox::new(&paths.state_dir);
    let (mut tail, seed) = EventTail::open(paths.events_log());
    engine.seed_events(&seed);
    engine.set_hooks_installed(tail.exists());
    let cache = AddressCache::new(paths.ble_address_file());
    let link_opts = LinkOptions::default();

    let mut claude = usage::find_claude(engine.settings().claude_path.as_deref(), &paths.home);
    match &claude {
        Some(c) => log::info!("claude CLI: {}", c.display()),
        None => log::warn!(
            "claude CLI not found (set claude_path in {}); usage comes from the usage endpoint every 12 min",
            paths.config_file.display()
        ),
    }
    engine.set_claude_found(claude.is_some());

    let mut link: Option<DeviceLink> = None;
    let mut device_events: Option<DeviceEvents> = None;
    let mut connecting: Option<JoinHandle<ConnectResult>> = None;
    let mut polling: Option<JoinHandle<PollResults>> = None;
    // Codex accounts poll on their own task, so a slow endpoint never holds
    // up a Claude account. The credits cache travels with it.
    let mut codex_polling: Option<JoinHandle<CodexResults>> = None;
    let mut credits = Some(CodexCache::default());
    let mut link_state = LinkState::Starting;
    let mut reconnect = Reconnect::default();
    let bt = Bluetooth::default();
    let mut wakeups = clawdmeter_ble::spawn_watcher(bt.clone());
    let mut last_settings = unix_now();
    let mut last_link_check = unix_now();
    let mut last_write_at = None;
    let mut device: Option<DeviceInfo> = None;
    let mut update: Option<UpdateRun> = None;
    let mut firmware_update: Option<FirmwareUpdate> = None;
    let mut firmware_update_id = 0;
    // When the current link came up, and its first successful write.
    let mut linked_at: Option<i64> = None;
    let mut wrote_at: Option<i64> = None;
    let mut ticker = tokio::time::interval(TICK);
    ticker.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);

    loop {
        ticker.tick().await;
        let now = unix_now();

        // ---- commands ----
        loop {
            match commands.try_recv() {
                Ok(Command::RefreshNow) => engine.request_refresh(now),
                Ok(cmd @ (Command::Reconnect | Command::Forget)) => {
                    if let Some(l) = link.take() {
                        l.close().await;
                    }
                    if cmd == Command::Forget {
                        if let Some(key) = cache.load() {
                            cache.invalidate(&key);
                        }
                    }
                    device_events = None;
                    device = None;
                    link_state = LinkState::Searching;
                    reconnect.retry_in(now, 0);
                }
                Ok(Command::ReloadSettings) => {
                    // This tick's write check sees the new settings.
                    last_settings = now;
                    reload_settings(&paths, &mut engine, &mut claude);
                }
                Ok(Command::UpdateFirmware(req)) => {
                    // A new request may replace one waiting on the restart
                    // (the user retried), not one still sending.
                    if update.is_some() {
                        log::warn!("firmware update: one is already sending; ignoring {}", req.version);
                        continue;
                    }
                    firmware_update_id = req.id;
                    firmware_update = Some(match start_update(link.as_ref(), &req) {
                        Ok(run) => {
                            let version = run.version.clone();
                            update = Some(run);
                            FirmwareUpdate::Starting { version }
                        }
                        Err(error) => {
                            log::warn!("firmware update: {error}");
                            FirmwareUpdate::Failed { version: req.version, error, at: now }
                        }
                    });
                }
                Ok(Command::CancelFirmwareUpdate) => {
                    if let Some(u) = &update {
                        log::info!("firmware update: cancelling");
                        u.cancel.store(true, Ordering::Relaxed);
                    }
                }
                Ok(Command::Shutdown) | Err(mpsc::error::TryRecvError::Disconnected) => {
                    if let Some(l) = link.take() {
                        l.close().await;
                    }
                    log::info!("daemon stopped");
                    return;
                }
                Err(mpsc::error::TryRecvError::Empty) => break,
            }
        }

        // ---- config (re-read every SETTINGS_EVERY, and on ReloadSettings:
        // this catches hand edits and a `--daemon` with no tray) ----
        if now - last_settings >= SETTINGS_EVERY {
            last_settings = now;
            reload_settings(&paths, &mut engine, &mut claude);
        }

        // ---- fast clock ----
        if let Some(a) = appearance.known().filter(|a| shown_appearance != Some(*a)) {
            log::info!("system appearance: {a:?}");
            shown_appearance = Some(a);
            engine.set_appearance(a);
        }
        for ev in tail.poll() {
            log::debug!("hook {:?} {}", ev.kind, ev.session);
            engine.on_hook_event(&ev, now);
        }
        engine.set_hooks_installed(tail.exists());
        let alert = inbox.take_alert(SystemTime::now());
        let clears = inbox.take_clears();
        if alert.is_some() || !clears.is_empty() {
            engine.on_attention(alert, &clears);
        }
        if engine.take_desktop_chime() {
            crate::chime::play();
        }
        if engine.expire_attention(SystemTime::now()) {
            log::info!("attention alert expired");
        }

        // ---- slow clock ----
        if let Some(rx) = device_events.as_mut() {
            while let Ok(ev) = rx.try_recv() {
                match ev {
                    DeviceEvent::Refresh => {
                        log::info!("refresh requested by device");
                        engine.request_refresh(now);
                    }
                    DeviceEvent::Dismissed => {
                        log::info!("attention alert dismissed on the device");
                        engine.on_device_dismissed();
                    }
                }
            }
        }
        if polling.as_ref().is_some_and(JoinHandle::is_finished) {
            if let Ok(results) = polling.take().expect("checked").await {
                for (d, r) in &results {
                    if let Err(e) = &r.result {
                        log::warn!("{}: {e}", d.display());
                    }
                }
                engine.on_poll_results(unix_now(), results);
                let blocks = engine.blocks(unix_now());
                if blocks != saved_blocks {
                    if let Err(e) = backoff::save_blocks(&paths.usage_blocks_file(), &blocks) {
                        log::warn!("couldn't save usage blocks: {e}");
                    }
                    saved_blocks = blocks;
                }
            }
        }
        if polling.is_none() {
            if let Some(plan) = engine.begin_poll(now) {
                log::debug!(
                    "polling ({:?}): {:?}",
                    plan.reason,
                    plan.accounts.iter().zip(&plan.plans).collect::<Vec<_>>()
                );
                let (c, home, cwd) = (claude.clone(), paths.home.clone(), paths.state_dir.clone());
                polling = Some(tokio::task::spawn_blocking(move || poll_all(c, plan, home, cwd)));
            }
        }
        if codex_polling.as_ref().is_some_and(JoinHandle::is_finished) {
            match codex_polling.take().expect("checked").await {
                Ok((results, cache)) => {
                    credits = Some(cache);
                    for (d, r) in &results {
                        if let Err(e) = &r.result {
                            log::warn!("{}: {e}", d.display());
                        } else if let Some(e) = &r.idle_miss {
                            log::warn!("{}: {e}; using the Codex session log", d.display());
                        }
                    }
                    engine.on_codex_results(unix_now(), results);
                    let blocks = engine.blocks(unix_now());
                    if blocks != saved_blocks {
                        if let Err(e) = backoff::save_blocks(&paths.usage_blocks_file(), &blocks) {
                            log::warn!("couldn't save usage blocks: {e}");
                        }
                        saved_blocks = blocks;
                    }
                }
                Err(e) => {
                    log::error!("Codex poll task failed: {e}");
                    credits = Some(CodexCache::default());
                }
            }
        }
        if codex_polling.is_none() {
            if let Some(dirs) = engine.begin_codex_poll(now) {
                log::debug!("polling Codex: {dirs:?}");
                let cache = credits.take().unwrap_or_default();
                codex_polling = Some(tokio::task::spawn_blocking(move || poll_codex(dirs, cache)));
            }
        }

        // ---- link ----
        if connecting.as_ref().is_some_and(JoinHandle::is_finished) {
            match connecting.take().expect("checked").await {
                Ok(Ok((l, rx))) => {
                    log::info!("connected to {} ({})", l.name().unwrap_or("device"), l.key());
                    device = l.info().cloned();
                    if let Some(i) = &device {
                        log::info!(
                            "board: {} {}{}",
                            i.env,
                            i.ver,
                            if i.can_update() { ", updates over Bluetooth" } else { "" }
                        );
                        // What the update nudge compares the release with.
                        let rec = crate::ipc::FirmwareRecord { env: i.env.clone(), ver: i.ver.clone() };
                        if let Err(e) = crate::ipc::save_firmware_record(&paths, &rec) {
                            log::warn!("couldn't save the board's firmware version: {e}");
                        }
                    }
                    linked_at = Some(now);
                    wrote_at = None;
                    link_state = LinkState::Connected { key: l.key().to_owned(), name: l.name().map(str::to_owned) };
                    link = Some(l);
                    device_events = Some(rx);
                    reconnect.connected();
                    inbox.reset();
                    engine.on_connected();
                }
                Ok(Err(e)) => {
                    link_state = match &e {
                        LinkError::NoAdapter(s) => LinkState::NoAdapter(s.clone()),
                        _ => LinkState::Searching,
                    };
                    let wait = reconnect.failed(now, e.reached_device());
                    if !matches!(e, LinkError::NotFound) || wait == 1 {
                        log::info!("{e}; retrying in {wait}s");
                    }
                }
                Err(e) => log::error!("connect task failed: {e}"),
            }
        }
        // The OS saw the device come back (BlueZ reconnected the bonded
        // device, or a scan saw it advertise): don't sit out the wait.
        while let Ok(w) = wakeups.try_recv() {
            if link.is_some() || !should_wake(&w, cache.load().as_deref()) {
                continue;
            }
            if reconnect.wake(now, connecting.is_some(), w.sighting) {
                let how = match w.sighting {
                    Sighting::Connected => "connected at the OS level",
                    Sighting::Advertising => "advertising",
                };
                log::info!("device {} {how}; reconnecting now", w.key);
            }
        }
        if link.is_none() && connecting.is_none() && reconnect.due(now) {
            if link_state == LinkState::Starting {
                link_state = LinkState::Connecting;
            }
            let (bt, cache, opts) = (bt.clone(), cache.clone(), link_opts.clone());
            connecting = Some(tokio::spawn(async move { DeviceLink::open(&bt, &cache, &opts).await }));
        }
        if let Some(l) = &link {
            if now - last_link_check >= LINK_CHECK_EVERY {
                last_link_check = now;
                if !l.is_connected().await {
                    log::info!("device disconnected, reconnecting...");
                    drop_link(&mut link).await;
                    device_events = None;
                    device = None;
                    link_state = LinkState::Searching;
                    reconnect.retry_in(now, 2);
                }
            }
        }

        // ---- firmware update ----
        if let Some(u) = &update {
            if u.task.is_finished() {
                let u = update.take().expect("checked");
                let version = u.version.clone();
                let result = u.task.await.unwrap_or_else(|e| Err(UpdateError::Link(e.to_string())));
                firmware_update = Some(match result {
                    // The board restarts into it (and drops the link): wait
                    // for it to come back and say which version it runs.
                    Ok(()) | Err(UpdateError::LostAfterEnd) => {
                        log::info!("firmware update: {version} sent; the board restarts into it");
                        if let Some(l) = link.take() {
                            l.close().await;
                        }
                        device_events = None;
                        device = None;
                        link_state = LinkState::Searching;
                        reconnect.retry_in(now, 3);
                        FirmwareUpdate::Restarting { version, since: now }
                    }
                    Err(e) => {
                        log::warn!("firmware update: {e}");
                        FirmwareUpdate::Failed { version, error: e.to_string(), at: now }
                    }
                });
            } else {
                let p = u.progress.lock().map(|g| *g).unwrap_or(UpdateProgress::Starting);
                let version = u.version.clone();
                firmware_update = Some(match p {
                    UpdateProgress::Starting => FirmwareUpdate::Starting { version },
                    UpdateProgress::Confirming { .. } => FirmwareUpdate::Confirming { version },
                    UpdateProgress::Sending { done, total } => FirmwareUpdate::Sending { version, done, total },
                    UpdateProgress::Verifying => FirmwareUpdate::Verifying { version },
                });
            }
        }
        if link.is_none() {
            linked_at = None;
            wrote_at = None;
        }
        match &firmware_update {
            Some(FirmwareUpdate::Restarting { version, since }) => {
                if let Some(next) = restart_check(version, *since, now, device.as_ref(), linked_at, wrote_at) {
                    log::info!("firmware update: {next:?}");
                    firmware_update = Some(next);
                }
            }
            Some(FirmwareUpdate::Done { at, .. } | FirmwareUpdate::Failed { at, .. })
                if now - at > UPDATE_SHOWN_SECS =>
            {
                firmware_update = None
            }
            _ => {}
        }

        // ---- write ----
        // Nothing else goes to the board while an update streams.
        if let Some(l) = link.as_ref().filter(|_| update.is_none()) {
            if let Some(w) = engine
                .next_write(now, HostClock { utc_offset_secs: clawdmeter_core::platform::utc_offset_secs(), ..host })
            {
                let bytes = w.payload.encode(MAX_WRITE);
                let label = match w.kind {
                    WriteKind::Poll => "Sending",
                    WriteKind::Heartbeat => "Heartbeat",
                    WriteKind::Attention => "Attention",
                    WriteKind::Event => "Event",
                    WriteKind::NoData => "No data",
                };
                log::info!("{label}: {}", String::from_utf8_lossy(&bytes));
                match l.write(&bytes).await {
                    Ok(()) => {
                        engine.mark_written(now, &w);
                        last_write_at = Some(now);
                        wrote_at.get_or_insert(now);
                    }
                    Err(e) => {
                        log::warn!("write failed: {e}");
                        if !l.is_connected().await {
                            drop_link(&mut link).await;
                            device_events = None;
                            device = None;
                            link_state = LinkState::Searching;
                            reconnect.retry_in(now, 2);
                        }
                    }
                }
            }
        }

        let _ = status.send(Status {
            link: link_state.clone(),
            engine: engine.status(now),
            last_write_at,
            claude: claude.clone(),
            device: device.clone(),
            firmware_update: firmware_update.clone(),
            firmware_update_id,
        });
    }
}

/// Drop a link that stopped working. BlueZ still reports a zombie
/// connected, so it is disconnected first (bounded at 5 s), or the next
/// attempt would find it connected and reuse it.
async fn drop_link(link: &mut Option<DeviceLink>) {
    if let Some(l) = link.take().filter(DeviceLink::is_zombie) {
        l.close().await;
    }
}

/// One poll of every account, printed as the payload the device would get
/// (`clawdmeter --once`). No BLE.
pub fn once(paths: &Paths) -> i32 {
    let settings = match Settings::load(paths) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("config: {e}");
            return 1;
        }
    };
    let claude = usage::find_claude(settings.claude_path.as_deref(), &paths.home);
    let now = unix_now();
    let mut engine = Engine::new(settings.clone());
    engine.set_appearance(clawdmeter_core::platform::system_appearance().unwrap_or_default());
    engine.restore_blocks(&backoff::load_blocks(&paths.usage_blocks_file()), now);
    // A blocked account isn't asked: a call during a block lengthens it.
    let blocked = engine.blocks(now);
    for (d, b) in &blocked {
        let provider = if settings.codex_dirs.contains(d) { usage::Provider::Codex } else { usage::Provider::Claude };
        let paused = usage::paused_message_by(provider, b.until, clawdmeter_core::platform::utc_offset_secs());
        println!("{}: {paused}", d.display());
    }
    // Nothing is known yet, so the idle source for every account (no message).
    let accounts: Vec<PathBuf> = settings.config_dirs.iter().filter(|d| !blocked.contains_key(*d)).cloned().collect();
    let plan = PollPlan {
        reason: clawdmeter_core::cadence::PollReason::Startup,
        plans: vec![probe::Plan::Usage; accounts.len()],
        accounts,
    };
    let results = poll_all(claude, plan, paths.home.clone(), paths.state_dir.clone());
    let codex_dirs: Vec<PathBuf> = settings.codex_dirs.iter().filter(|d| !blocked.contains_key(*d)).cloned().collect();
    let (codex_results, _) = poll_codex(codex_dirs, CodexCache::default());
    let mut ok = false;
    for (d, r) in &results {
        match &r.result {
            Ok(r) if r.is_spend_only() => {
                ok = true;
                println!("{}: spend limit {}% used", d.display(), r.session_pct());
            }
            Ok(r) if r.no_session_limit() => {
                ok = true;
                println!("{}: no 5-hour limit, week {}%", d.display(), r.session_pct());
            }
            Ok(r) => {
                ok = true;
                println!(
                    "{}: session {}%{}, week {}%",
                    d.display(),
                    r.session_pct(),
                    if r.window_started() { "" } else { " (window not started)" },
                    r.week.as_ref().map_or(0, |w| w.pct)
                );
            }
            Err(e) => println!("{}: {e}", d.display()),
        }
    }
    for (d, polled) in &codex_results {
        match &polled.result {
            Ok(r) => {
                ok = true;
                let resets = r.resets.as_ref().map_or_else(
                    || "reset credits unknown".to_owned(),
                    |c| {
                        let mut s = format!("{} reset credit(s) held", c.held);
                        if let Some(t) = c.next_expiry() {
                            s += &format!(", next expires {}", t.format("%Y-%m-%d %H:%M UTC"));
                        }
                        s
                    },
                );
                let windows = match &r.session {
                    Some(w) => format!("5-hour {}%, week {}%", w.pct, r.week.as_ref().map_or(0, |w| w.pct)),
                    None => format!("no 5-hour limit, week {}%", r.session_pct()),
                };
                let source = match &polled.idle_miss {
                    Some(e) => format!(" (from the session log: {e})"),
                    None => String::new(),
                };
                println!("{} (Codex): {windows}, {resets}{source}", d.display());
                println!(
                    "  Codex payload: {}",
                    clawdmeter_core::Payload::from_reading(r, chrono::Utc::now()).to_json()
                );
            }
            Err(e) => println!("{} (Codex): {e}", d.display()),
        }
    }
    engine.on_poll_results(now, results);
    engine.on_codex_results(now, codex_results);
    if let Err(e) = backoff::save_blocks(&paths.usage_blocks_file(), &engine.blocks(unix_now())) {
        eprintln!("couldn't save usage blocks: {e}");
    }
    let host = HostClock {
        utc_offset_secs: clawdmeter_core::platform::utc_offset_secs(),
        prefers_12h: clawdmeter_core::platform::prefers_12h(),
    };
    if let Some(w) = engine.next_write(now, host) {
        println!("{}", String::from_utf8_lossy(&w.payload.encode(MAX_WRITE)));
    }
    if ok {
        0
    } else {
        1
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn info(ver: &str) -> DeviceInfo {
        DeviceInfo {
            env: "waveshare_amoled_216".into(),
            ver: ver.into(),
            rev: String::new(),
            ota_slot: 1,
            ota_proto: 1,
        }
    }

    #[test]
    fn a_reconnect_after_an_update_checks_the_version() {
        let check = |info: Option<&DeviceInfo>, now: i64, linked: Option<i64>, wrote: Option<i64>| {
            restart_check("v1.10.0", 100, now, info, linked, wrote)
        };
        let new = info("1.10.0");
        let old = info("v1.9.0");
        // The new version, with or without the v: done once a payload went
        // through and the link held (or the link held a while on its own).
        assert_eq!(check(Some(&new), 105, Some(104), None), None, "just connected");
        assert_eq!(check(Some(&new), 120, Some(104), Some(106)), None, "written, not settled yet");
        assert_eq!(
            check(Some(&new), 126, Some(110), Some(106)),
            Some(FirmwareUpdate::Done { version: "v1.10.0".into(), at: 126 })
        );
        assert!(matches!(check(Some(&new), 134, Some(104), None), Some(FirmwareUpdate::Done { .. })));
        // The old version right after DONE may be the old firmware before its
        // restart; later, it went back.
        assert_eq!(check(Some(&old), 105, Some(104), Some(104)), None);
        let Some(FirmwareUpdate::Failed { error, .. }) = check(Some(&old), 121, Some(115), None) else {
            panic!("still the old version after the settle time")
        };
        assert!(error.contains("didn't take") && error.contains("v1.9.0"), "{error}");
        // No INFO (disconnected, or the read failed): wait, then say which.
        assert_eq!(check(None, 200, None, None), None);
        let Some(FirmwareUpdate::Failed { error, .. }) = check(None, 221, None, None) else { panic!() };
        assert!(error.contains("hasn't reconnected"), "{error}");
        let Some(FirmwareUpdate::Failed { error, .. }) = check(None, 221, Some(150), None) else { panic!() };
        assert!(error.contains("didn't say"), "{error}");
    }

    #[test]
    fn only_a_finished_update_lets_another_start() {
        let v = || "v1".to_string();
        assert!(FirmwareUpdate::Starting { version: v() }.running());
        assert!(FirmwareUpdate::Confirming { version: v() }.running());
        assert!(FirmwareUpdate::Restarting { version: v(), since: 0 }.running());
        assert!(!FirmwareUpdate::Done { version: v(), at: 0 }.running());
        assert!(!FirmwareUpdate::Failed { version: v(), error: String::new(), at: 0 }.running());
    }

    #[test]
    fn a_request_needs_a_board_that_takes_updates() {
        let req = FirmwareRequest {
            image: "/nonexistent".into(),
            env: "waveshare_amoled_216".into(),
            version: "v1.10.0".into(),
            sha256: String::new(),
            id: 1,
        };
        let err = start_update(None, &req).err().unwrap();
        assert_eq!(err, "The board isn't connected over Bluetooth.");
    }
}
