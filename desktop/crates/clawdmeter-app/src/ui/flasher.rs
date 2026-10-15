//! The flasher: plug in, confirm the board, flash. It is the
//! Flash page, and the setup wizard's board and flash steps.
//!
//! Steps: Connect → Identify → Firmware → Flash → Set up. Detection is the
//! three probes from the spec, cheapest first: the firmware's `info` command,
//! then chip and flash size from the ROM, then the user picks among what's
//! left, shown at true aspect ratio. Images come from the latest GitHub
//! Release (or a local release folder with a manifest.json) and are checked
//! against the manifest's sha256 before anything is written.

use std::path::PathBuf;
use std::sync::Arc;
use std::time::Duration;

use clawdmeter_core::{Paths, Settings};
use clawdmeter_flash::device::{self, ChipInfo, DeviceError, FirmwareInfo, FlashProgress, Identification};
use clawdmeter_flash::release::{self, Release};
use clawdmeter_flash::{BoardImage, Manifest};
use futures::StreamExt as _;
use gpui::{div, img, prelude::*, px, Context, EventEmitter, PathPromptOptions, SharedString, Window};

use super::theme::{Theme, FONT_DISPLAY, FONT_MONO, R_M, R_PILL, R_S};
use super::widgets::{bar, button, caption, card, clawd, icon, kv, notice, para, pill, ButtonKind, Tone};
use super::{command, live_status, run_blocking, Navigate, REFRESH};
use crate::boards::{self, BoardInfo};
use crate::ipc::{self, AppCommand, FirmwareRecord};
use crate::Page;

const STEPS: [&str; 5] = ["Connect", "Identify", "Firmware", "Flash", "Done"];

/// Where images come from.
#[derive(Clone)]
enum Source {
    Remote(Release),
    Local { dir: PathBuf, manifest: Manifest },
}

impl Source {
    fn manifest(&self) -> &Manifest {
        match self {
            Source::Remote(r) => &r.manifest,
            Source::Local { manifest, .. } => manifest,
        }
    }
    fn label(&self, home: &std::path::Path) -> String {
        match self {
            Source::Remote(_) => "GitHub".into(),
            Source::Local { dir, .. } => crate::tilde(&dir.display().to_string(), home),
        }
    }
    fn version(&self) -> String {
        let v = &self.manifest().version;
        if v.starts_with('v') {
            v.clone()
        } else {
            format!("v{v}")
        }
    }
}

/// Read a local release folder's manifest.json.
fn load_local(path: &std::path::Path) -> Result<Source, (String, bool)> {
    std::fs::read_to_string(path)
        .map_err(|e| format!("Couldn't open {}. {}", path.display(), crate::errors::io_message(&e)))
        .and_then(|s| {
            Manifest::parse(&s).map_err(|e| {
                log::warn!("{}: {e}", path.display());
                "This file isn't a valid Clawdmeter release manifest.".to_string()
            })
        })
        .map(|manifest| Source::Local { dir: path.parent().map(PathBuf::from).unwrap_or_default(), manifest })
        .map_err(|m| (m, false))
}

/// Where the flow currently is.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Step {
    /// Waiting for a board on USB.
    Connect,
    /// Asking the board who it is / picking among candidates.
    Identify,
    /// Downloading and checking the image.
    Firmware,
    /// Writing it.
    Flash,
    /// Written and restarted.
    Done,
}

/// Where the flasher is shown.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FlashMode {
    /// The Flash page.
    Page,
    /// Inside the setup wizard: no step header, no done screen, and a board
    /// that already runs the latest firmware can skip flashing.
    Setup,
}

/// What the setup wizard listens for.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FlashEvent {
    /// The board was flashed and restarted.
    Flashed,
    /// The board already runs the latest firmware; nothing was written.
    UpToDate,
}

impl Step {
    fn index(self) -> usize {
        self as usize
    }
}

enum FlashMsg {
    Log(String),
    Progress(f32),
    Finished(Result<(), String>),
}

/// What the flasher needs from USB. [`Usb`] is the real thing; tests drive
/// the window with a fake.
pub trait FlashHardware: Send + Sync + 'static {
    /// Attached Espressif ports.
    fn ports(&self) -> Vec<String>;
    /// Probe 1: ask running firmware who it is.
    fn probe_info(&self, port: &str) -> Option<FirmwareInfo>;
    /// Probe 2: chip and flash size from the ROM bootloader.
    fn probe_chip(&self, port: &str) -> Result<ChipInfo, DeviceError>;
    /// Write a merged image at 0x0 and reset into it.
    fn flash(
        &self,
        port: &str,
        image: &[u8],
        board: &BoardImage,
        progress: &mut dyn FnMut(FlashProgress),
    ) -> Result<(), DeviceError>;
}

/// The board on USB, through `clawdmeter_flash::device`.
pub struct Usb;

impl FlashHardware for Usb {
    fn ports(&self) -> Vec<String> {
        device::list_ports().into_iter().map(|p| p.path).collect()
    }
    fn probe_info(&self, port: &str) -> Option<FirmwareInfo> {
        device::probe_info(port, Duration::from_millis(1500)).ok().flatten()
    }
    fn probe_chip(&self, port: &str) -> Result<ChipInfo, DeviceError> {
        device::probe_chip(port)
    }
    fn flash(
        &self,
        port: &str,
        image: &[u8],
        board: &BoardImage,
        progress: &mut dyn FnMut(FlashProgress),
    ) -> Result<(), DeviceError> {
        device::flash_merged(port, image, board, progress)
    }
}

/// The flasher.
pub struct FlasherView {
    paths: Paths,
    hw: Arc<dyn FlashHardware>,
    step: Step,
    mode: FlashMode,
    /// On screen: only then does it listen on USB.
    active: bool,
    /// The release check has been started (it runs on first show).
    requested: bool,
    /// "Don't have one?": the supported boards with where to buy them.
    shop: bool,
    /// The firmware step's file, source and port details are open.
    details: bool,
    /// `None` while checking; `Err((message, no_release_yet))`.
    source: Option<Result<Source, (String, bool)>>,
    port: Option<String>,
    probing: bool,
    info: Option<FirmwareInfo>,
    chip: Option<ChipInfo>,
    candidates: Vec<String>,
    pick: Option<String>,
    image: Option<Result<Vec<u8>, String>>,
    download: f32,
    progress: f32,
    log: Vec<String>,
    flashing: bool,
    error: Option<String>,
    connected: bool,
    ticks: u32,
}

impl EventEmitter<Navigate> for FlasherView {}
impl EventEmitter<FlashEvent> for FlasherView {}

/// A board's illustration in a `w`×`h` box.
pub fn board_art(b: &BoardInfo, w: f32, h: f32) -> impl IntoElement {
    div()
        .flex()
        .flex_shrink_0()
        .items_center()
        .justify_center()
        .w(px(w))
        .h(px(h))
        .child(img(b.illustration()).w(px(w)).h(px(h)))
}

/// Every supported board with a link to buy it.
pub fn shop(t: &Theme) -> impl IntoElement {
    let mut grid = div().flex().flex_wrap().gap(px(10.));
    for (i, b) in boards::BOARDS.iter().enumerate() {
        let url = b.buy_url;
        grid = grid.child(
            div()
                .id(SharedString::from(format!("buy-{i}")))
                .debug_selector(move || format!("buy-{i}"))
                .flex()
                .flex_col()
                .items_center()
                .gap(px(2.))
                .w(px(146.))
                .pt(px(8.))
                .pb(px(12.))
                .rounded(px(R_M))
                .bg(t.panel)
                .cursor_pointer()
                .hover(|s| s.bg(t.track))
                .on_click(move |_, _, cx| cx.open_url(url))
                .child(board_art(b, 120., 110.))
                .child(div().text_size(px(13.5)).child(b.name))
                .child(div().text_size(px(11.5)).text_color(t.mute).child(b.summary))
                .child(div().pt(px(4.)).text_size(px(12.5)).text_color(t.accent).child("Buy ↗")),
        );
    }
    grid
}

impl FlasherView {
    /// Build. The release manifest is fetched and USB watched once the
    /// flasher is on screen ([`FlasherView::set_active`]). `manifest`: a local
    /// release `manifest.json` to use instead of GitHub.
    pub fn new(paths: Paths, manifest: Option<PathBuf>, cx: &mut Context<Self>) -> Self {
        Self::with_hardware(paths, manifest, Arc::new(Usb), cx)
    }

    /// [`FlasherView::new`] with the USB side swapped out.
    pub fn with_hardware(
        paths: Paths,
        manifest: Option<PathBuf>,
        hw: Arc<dyn FlashHardware>,
        cx: &mut Context<Self>,
    ) -> Self {
        cx.spawn(async move |this, cx| loop {
            cx.background_executor().timer(REFRESH).await;
            if this.update(cx, |v, cx| v.tick(cx)).is_err() {
                break;
            }
        })
        .detach();
        let mut v = FlasherView {
            paths,
            hw,
            step: Step::Connect,
            mode: FlashMode::Page,
            active: false,
            requested: false,
            shop: false,
            details: false,
            source: None,
            port: None,
            probing: false,
            info: None,
            chip: None,
            candidates: Vec::new(),
            pick: None,
            image: None,
            download: 0.,
            progress: 0.,
            log: Vec::new(),
            flashing: false,
            error: None,
            connected: false,
            ticks: 0,
        };
        if let Some(m) = manifest {
            v.source = Some(load_local(&m));
            v.requested = true;
        }
        v
    }

    /// Show or hide the flasher. Shown, it checks for the release (once)
    /// and listens for a board on USB.
    pub fn set_active(&mut self, active: bool, cx: &mut Context<Self>) {
        self.active = active;
        if active && !self.requested {
            self.requested = true;
            self.fetch_release(cx);
        }
        cx.notify();
    }

    /// Page or setup-wizard presentation.
    pub fn set_mode(&mut self, mode: FlashMode, cx: &mut Context<Self>) {
        self.mode = mode;
        cx.notify();
    }

    /// Where the flow is.
    pub fn step(&self) -> Step {
        self.step
    }

    /// Pick a board among the candidates and fetch its image (what the
    /// picker and Continue do).
    pub fn choose(&mut self, env: &str, cx: &mut Context<Self>) {
        self.pick = Some(env.to_owned());
        self.start_download(cx);
    }

    /// Write the fetched image (what the Flash button does).
    pub fn flash(&mut self, cx: &mut Context<Self>) {
        self.start_flash(cx);
    }

    /// Open or close the "Don't have one?" board list.
    pub fn show_shop(&mut self, open: bool, cx: &mut Context<Self>) {
        self.shop = open;
        cx.notify();
    }

    /// The board picked (or identified) in this run.
    pub fn picked(&self) -> Option<&str> {
        self.pick.as_deref()
    }

    /// Boards the identified chip could be.
    pub fn candidates(&self) -> &[String] {
        &self.candidates
    }

    /// Writing to the board right now (don't walk away).
    pub fn flashing(&self) -> bool {
        self.flashing
    }

    /// The fetched image is ready to write.
    pub fn image_ready(&self) -> bool {
        matches!(self.image, Some(Ok(_)))
    }

    /// The board on USB already runs this release (or newer).
    pub fn up_to_date(&self) -> bool {
        let (Some(info), Some(Ok(source))) = (&self.info, &self.source) else { return false };
        info.ver.trim_start_matches(['v', 'V']).starts_with(|c: char| c.is_ascii_digit())
            && !clawdmeter_flash::manifest::is_newer(&source.manifest().version, &info.ver)
    }

    fn fetch_release(&mut self, cx: &mut Context<Self>) {
        self.source = None;
        let repo = Settings::load(&self.paths)
            .map(|s| s.release_repo)
            .unwrap_or_else(|_| clawdmeter_flash::DEFAULT_RELEASE_REPO.into());
        run_blocking(
            cx,
            move || {
                release::release_for_app(&repo, crate::version()).map_err(|e| {
                    log::warn!("release check for {repo}: {e} ({})", e.detail().unwrap_or_default());
                    let custom = repo != clawdmeter_flash::DEFAULT_RELEASE_REPO;
                    let none_yet = matches!(e, release::ReleaseError::NoRelease);
                    let msg = match (none_yet, custom) {
                        (true, true) => format!("No firmware release was found in {repo}."),
                        _ => e.to_string(),
                    };
                    (msg, none_yet)
                })
            },
            |v, r, cx| {
                if let Ok(rel) = &r {
                    let _ = std::fs::create_dir_all(&v.paths.cache_dir);
                    let _ = std::fs::write(ipc::latest_release_file(&v.paths), &rel.manifest.version);
                }
                v.source = Some(r.map(Source::Remote));
                if v.step == Step::Identify && v.candidates.is_empty() && !v.probing {
                    v.identify(cx);
                }
            },
        );
    }

    fn open_local(&mut self, cx: &mut Context<Self>) {
        let rx = cx.prompt_for_paths(PathPromptOptions {
            files: true,
            directories: false,
            multiple: false,
            prompt: Some("Choose a Clawdmeter manifest.json".into()),
        });
        cx.spawn(async move |this, cx| {
            if let Ok(Ok(Some(mut paths))) = rx.await {
                let Some(path) = paths.pop() else { return };
                let _ = this.update(cx, |v, cx| {
                    v.source = Some(load_local(&path));
                    v.error = None;
                    if v.port.is_some() {
                        v.step = Step::Identify;
                        v.identify(cx);
                    }
                    cx.notify();
                });
            }
        })
        .detach();
    }

    fn tick(&mut self, cx: &mut Context<Self>) {
        self.ticks += 1;
        if self.active && self.step == Step::Connect && !self.probing && self.ticks.is_multiple_of(2) {
            if let Some(p) = self.hw.ports().into_iter().next() {
                self.port = Some(p);
                self.step = Step::Identify;
                self.identify(cx);
            }
        }
        if self.step == Step::Done {
            self.connected = live_status(&self.paths).is_some_and(|s| s.link == "connected");
        }
        if self.active {
            cx.notify();
        }
    }

    fn identify(&mut self, cx: &mut Context<Self>) {
        let Some(port) = self.port.clone() else { return };
        let Some(Ok(source)) = self.source.clone() else { return };
        self.probing = true;
        self.error = None;
        self.candidates.clear();
        self.pick = None;
        self.info = None;
        self.chip = None;
        let hw = self.hw.clone();
        run_blocking(
            cx,
            move || -> Result<(Option<FirmwareInfo>, Option<ChipInfo>, Identification), String> {
                let manifest = source.manifest();
                // Probe 1: running firmware names its env.
                if let Some(info) = hw.probe_info(&port) {
                    if manifest.board(&info.env).is_some() {
                        return Ok((Some(info.clone()), None, Identification::Firmware(info)));
                    }
                    log::info!("This release has no firmware for the detected board: {}.", info.env);
                }
                // Probe 2: chip and flash size from the ROM.
                let chip = hw.probe_chip(&port).map_err(|e| e.to_string())?;
                let id = device::identify_from_chip(manifest, &chip);
                Ok((None, Some(chip), id))
            },
            |v, r, _| {
                v.probing = false;
                match r {
                    Err(e) => v.error = Some(e),
                    Ok((info, chip, id)) => {
                        v.chip = chip;
                        if let Some(i) = &info {
                            let _ = ipc::save_firmware_record(
                                &v.paths,
                                &FirmwareRecord { env: i.env.clone(), ver: i.ver.clone() },
                            );
                        }
                        v.info = info;
                        match id {
                            Identification::Firmware(i) => {
                                v.candidates = vec![i.env.clone()];
                                v.pick = Some(i.env);
                            }
                            Identification::Unique(env) => {
                                v.candidates = vec![env.clone()];
                                v.pick = Some(env);
                            }
                            Identification::Choose(envs) => v.candidates = envs,
                            Identification::Unsupported(c) => {
                                v.error = Some(format!(
                                    "This board has an {}{}. None of the boards in this release match it.",
                                    device::pretty_chip(&c.chip),
                                    c.flash_mb.map(|m| format!(" with {m} MB of flash")).unwrap_or_default()
                                ))
                            }
                        }
                    }
                }
            },
        );
    }

    fn board_image(&self) -> Option<BoardImage> {
        let source = self.source.as_ref()?.as_ref().ok()?;
        source.manifest().board(self.pick.as_deref()?).cloned()
    }

    fn start_download(&mut self, cx: &mut Context<Self>) {
        let (Some(Ok(source)), Some(board)) = (self.source.clone(), self.board_image()) else { return };
        self.step = Step::Firmware;
        self.image = None;
        self.download = 0.;
        let cache = self.paths.cache_dir.clone();
        let (tx, mut rx) = futures::channel::mpsc::unbounded::<Result<f32, Result<Vec<u8>, String>>>();
        cx.background_executor()
            .spawn(async move {
                let r = match &source {
                    Source::Remote(rel) => release::fetch_image(rel, &board, &cache, |done, total| {
                        if total > 0 {
                            let _ = tx.unbounded_send(Ok(done as f32 / total as f32));
                        }
                    })
                    .map_err(|e| e.to_string()),
                    Source::Local { dir, .. } => {
                        let name = std::path::Path::new(&board.file).file_name().map(PathBuf::from).unwrap_or_default();
                        std::fs::read(dir.join(name))
                            .map_err(|e| match e.kind() {
                                std::io::ErrorKind::NotFound => {
                                    format!("Place {} in the same folder as manifest.json.", board.file)
                                }
                                _ => format!("Couldn't read {}. {}", board.file, crate::errors::io_message(&e)),
                            })
                            .and_then(|d| release::verify(&board, &d).map(|_| d).map_err(|e| e.to_string()))
                    }
                };
                let _ = tx.unbounded_send(Err(r));
            })
            .detach();
        cx.spawn(async move |this, cx| {
            while let Some(m) = rx.next().await {
                let stop = m.is_err();
                let _ = this.update(cx, |v, cx| {
                    match m {
                        Ok(f) => v.download = f,
                        Err(r) => {
                            v.download = 1.;
                            v.image = Some(r);
                        }
                    }
                    cx.notify();
                });
                if stop {
                    break;
                }
            }
        })
        .detach();
    }

    fn start_flash(&mut self, cx: &mut Context<Self>) {
        let (Some(Ok(data)), Some(board), Some(port)) = (self.image.clone(), self.board_image(), self.port.clone())
        else {
            return;
        };
        let version = self.source.as_ref().and_then(|s| s.as_ref().ok()).map(Source::version).unwrap_or_default();
        self.step = Step::Flash;
        self.flashing = true;
        self.progress = 0.;
        self.error = None;
        self.log = vec![
            format!("USB port: {port}"),
            format!("{} ({:.1} MB), checksum verified", board.file, data.len() as f32 / 1_048_576.),
        ];
        let (tx, mut rx) = futures::channel::mpsc::unbounded::<FlashMsg>();
        let hw = self.hw.clone();
        cx.background_executor()
            .spawn(async move {
                let _ = tx.unbounded_send(FlashMsg::Log("Restarting the board into its bootloader…".into()));
                let mut total = data.len().max(1);
                let r = hw.flash(&port, &data, &board, &mut |p| {
                    let m = match p {
                        FlashProgress::Start { total: t } => {
                            total = t.max(1);
                            FlashMsg::Log(format!("Writing {:.1} MB…", t as f32 / 1_048_576.))
                        }
                        FlashProgress::Write { done } => FlashMsg::Progress(done as f32 / total as f32),
                        FlashProgress::Verify => FlashMsg::Log("Verifying firmware…".into()),
                        FlashProgress::Done => FlashMsg::Log("Restarting the board…".into()),
                    };
                    let _ = tx.unbounded_send(m);
                });
                let _ = tx.unbounded_send(FlashMsg::Finished(r.map_err(|e| e.to_string())));
            })
            .detach();
        let env = self.pick.clone().unwrap_or_default();
        cx.spawn(async move |this, cx| {
            while let Some(m) = rx.next().await {
                let fin = matches!(m, FlashMsg::Finished(_));
                let env = env.clone();
                let version = version.clone();
                let _ = this.update(cx, |v, cx| {
                    match m {
                        FlashMsg::Log(l) => v.log.push(l),
                        FlashMsg::Progress(p) => v.progress = p,
                        FlashMsg::Finished(Ok(())) => {
                            v.flashing = false;
                            v.progress = 1.;
                            v.log.push("Firmware installed".into());
                            let ver = version.trim_start_matches('v').to_owned();
                            let _ = ipc::save_firmware_record(&v.paths, &FirmwareRecord { env, ver });
                            command(&v.paths, AppCommand::Reconnect);
                            v.step = Step::Done;
                            cx.emit(FlashEvent::Flashed);
                        }
                        FlashMsg::Finished(Err(e)) => {
                            v.flashing = false;
                            v.log.push("Installation stopped.".into());
                            v.error = Some(e);
                        }
                    }
                    cx.notify();
                });
                if fin {
                    break;
                }
            }
        })
        .detach();
    }

    /// One step back (the wizard's Back). False at the first step.
    pub fn back(&mut self) -> bool {
        match self.step {
            Step::Connect => return false,
            Step::Identify | Step::Done => self.restart(),
            Step::Firmware => self.step = Step::Identify,
            Step::Flash if !self.flashing => self.step = Step::Firmware,
            Step::Flash => {}
        }
        true
    }

    /// Back to "plug it in".
    pub fn restart(&mut self) {
        self.step = Step::Connect;
        self.port = None;
        self.info = None;
        self.chip = None;
        self.candidates.clear();
        self.pick = None;
        self.image = None;
        self.error = None;
        self.log.clear();
        self.progress = 0.;
        self.ticks = 0;
    }

    // --------------------------------------------------------------- views

    fn steps(&self, t: &Theme) -> impl IntoElement {
        let mut row = div().flex().items_center().gap(px(6.)).flex_wrap();
        for (i, s) in STEPS.iter().enumerate() {
            let (done, on) = (i < self.step.index(), i == self.step.index());
            let c = if on {
                t.accent
            } else if done {
                t.ok
            } else {
                t.track
            };
            row = row.child(
                div()
                    .flex()
                    .items_center()
                    .gap(px(6.))
                    .px(px(10.))
                    .py(px(4.))
                    .rounded(px(R_PILL))
                    .when(on, |d| d.bg(t.panel))
                    .text_size(px(12.5))
                    .text_color(if on || done { t.text } else { t.mute })
                    .child(div().size(px(8.)).rounded(px(R_PILL)).bg(c))
                    .child(*s),
            );
        }
        row
    }

    fn source_problem(&self, t: &Theme, cx: &mut Context<Self>) -> Option<gpui::Div> {
        let (msg, none_yet) = match &self.source {
            None => return Some(div().flex().justify_center().child(caption(t, "Checking for firmware…"))),
            Some(Ok(_)) => return None,
            Some(Err(e)) => e.clone(),
        };
        let (me1, me2) = (cx.entity().downgrade(), cx.entity().downgrade());
        let text = if none_yet {
            format!("{msg} If you have a release folder, open its manifest.json to flash from it.")
        } else {
            msg
        };
        Some(
            notice(t, if none_yet { Tone::Plain } else { Tone::Warn }, text).child(
                div()
                    .flex()
                    .gap(px(8.))
                    .pl(px(18.))
                    .child(button(t, "local", "Open manifest.json…", ButtonKind::Small, true, move |_, _, cx| {
                        let _ = me2.update(cx, |v, cx| v.open_local(cx));
                    }))
                    .child(button(t, "retry-release", "Check again", ButtonKind::Small, true, move |_, _, cx| {
                        let _ = me1.update(cx, |v, cx| v.fetch_release(cx));
                    })),
            ),
        )
    }

    fn connect_view(&mut self, t: &Theme, cx: &mut Context<Self>) -> gpui::Div {
        let shop_open = self.shop;
        let mut d = div()
            .flex()
            .flex_col()
            .items_center()
            .gap(px(12.))
            .pt(px(18.))
            .child(clawd(4.))
            .child(div().font_family(FONT_DISPLAY).text_size(px(22.)).child("Connect your board"))
            .child(
                div()
                    .max_w(px(420.))
                    .text_center()
                    .child(para(t, "Connect the board with a USB data cable. A charge-only cable won't work.")),
            )
            .child(pill(t, "Waiting for USB", Tone::Accent))
            .child(
                div()
                    .id("shop-toggle")
                    .debug_selector(|| "shop-toggle".into())
                    .pt(px(8.))
                    .text_size(px(13.))
                    .text_color(t.accent)
                    .cursor_pointer()
                    .hover(|s| s.text_color(t.focus))
                    .on_click(cx.listener(|v, _, _, cx| {
                        v.shop = !v.shop;
                        cx.notify();
                    }))
                    .child(if shop_open { "Hide supported boards" } else { "View supported boards" }),
            );
        if shop_open {
            d = d.child(div().pt(px(4.)).child(shop(t)));
        }
        if let Some(p) = self.source_problem(t, cx) {
            d = d.child(div().w_full().pt(px(8.)).child(p));
        }
        d
    }

    fn identify_view(&mut self, t: &Theme, cx: &mut Context<Self>) -> gpui::Div {
        let chip_text = match (&self.info, &self.chip) {
            (Some(i), _) => format!("Running Clawdmeter {}", i.ver),
            (None, Some(c)) => format!(
                "{} · {}{}",
                c.chip.to_uppercase().replace("ESP32", "ESP32-"),
                c.flash_mb.map(|m| format!("{m} MB")).unwrap_or_else(|| "? MB".into()),
                if c.features.iter().any(|f| f.to_ascii_lowercase().contains("psram")) { " · PSRAM" } else { "" }
            ),
            (None, None) => String::new(),
        };
        // Firmware that names its board needs a confirmation, not a choice.
        let heading = if self.info.is_some() && !self.probing { "Your board" } else { "Choose your board" };
        let mut d = div().flex().flex_col().gap(px(12.)).child(
            div()
                .flex()
                .items_center()
                .justify_between()
                .child(div().font_family(FONT_DISPLAY).text_size(px(20.)).child(heading))
                .when(self.probing || !chip_text.is_empty(), |d| {
                    let (text, tone) = if self.probing {
                        ("Identifying board…".to_string(), Tone::Accent)
                    } else {
                        (chip_text, Tone::Ok)
                    };
                    d.child(pill(t, text, tone))
                }),
        );
        if let Some(p) = self.source_problem(t, cx) {
            return d.child(p);
        }
        if self.probing {
            return d.child(para(
                t,
                "Checking board details. A board without Clawdmeter firmware may restart into its bootloader.",
            ));
        }
        let latest = self.source.as_ref().and_then(|s| s.as_ref().ok()).map(Source::version).unwrap_or_default();
        let up_to_date = self.up_to_date();
        let hint = match (&self.info, self.candidates.len()) {
            (Some(_), _) if up_to_date => {
                format!("Firmware is up to date ({latest}). Continue, or reinstall if needed.")
            }
            (Some(_), _) => "Board identified from its Clawdmeter firmware.".to_string(),
            (None, 1) => "One board in this release matches the detected chip.".to_string(),
            (None, n) if n > 1 => format!("{n} boards match this chip. Select the model on your desk."),
            _ => String::new(),
        };
        if !hint.is_empty() {
            d = d.child(para(t, hint));
        }
        let manifest = self.source.as_ref().and_then(|s| s.as_ref().ok()).map(|s| s.manifest().clone());
        // Four even columns fit both the page and the wizard; more wrap.
        let mut grid = div().grid().grid_cols(4).gap(px(10.));
        for (i, env) in self.candidates.clone().into_iter().enumerate() {
            let info = boards::board(&env);
            // Our short names read better side by side than the manifest's
            // full ones; unknown envs fall back to the manifest.
            let name: SharedString = info
                .map(|b| b.name.to_owned())
                .or_else(|| manifest.as_ref().and_then(|m| m.board(&env)).map(|b| b.display_name.clone()))
                .filter(|n| !n.is_empty())
                .unwrap_or_else(|| env.clone())
                .into();
            let summary = info.map(|b| b.summary).unwrap_or("");
            let on = self.pick.as_deref() == Some(env.as_str());
            let me = cx.entity().downgrade();
            let e2 = env.clone();
            grid = grid.child(
                div()
                    .id(SharedString::from(format!("board-{i}")))
                    .debug_selector(|| format!("board-{i}"))
                    .flex()
                    .flex_col()
                    .items_center()
                    .gap(px(4.))
                    .min_w_0()
                    .pt(px(8.))
                    .px(px(10.))
                    .pb(px(12.))
                    .rounded(px(R_M))
                    .border_1()
                    .border_color(if on { t.accent } else { t.line })
                    .when(on, |d| d.bg(t.panel))
                    .cursor_pointer()
                    .hover(|s| s.bg(t.panel))
                    .on_click(move |_, _, cx| {
                        let env = e2.clone();
                        let _ = me.update(cx, |v, cx| {
                            v.pick = Some(env);
                            cx.notify();
                        });
                    })
                    .child(match info {
                        Some(b) => board_art(b, 120., 110.).into_any_element(),
                        None => div().h(px(110.)).flex().items_center().child(clawd(2.)).into_any_element(),
                    })
                    .child(div().text_size(px(13.5)).text_center().child(name))
                    .child(div().text_size(px(11.5)).text_color(t.mute).text_center().child(summary)),
            );
        }
        d = d.child(grid);
        let (me1, me2) = (cx.entity().downgrade(), cx.entity().downgrade());
        // In setup, an up-to-date board moves straight on; flashing it again
        // stays possible.
        let skip = up_to_date && self.mode == FlashMode::Setup;
        let mut actions = div().flex().items_center().gap(px(8.));
        if skip {
            actions = actions
                .child(button(t, "flash-anyway", "Reinstall firmware", ButtonKind::Small, true, move |_, _, cx| {
                    let _ = me2.update(cx, |v, cx| v.start_download(cx));
                }))
                .child(button(
                    t,
                    "continue",
                    "Continue",
                    ButtonKind::Primary,
                    true,
                    cx.listener(|_, _, _, cx| cx.emit(FlashEvent::UpToDate)),
                ));
        } else {
            let label = if up_to_date { "Reinstall firmware" } else { "Continue" };
            actions = actions.child(button(
                t,
                "continue",
                label,
                ButtonKind::Primary,
                self.pick.is_some(),
                move |_, _, cx| {
                    let _ = me2.update(cx, |v, cx| v.start_download(cx));
                },
            ));
        }
        // In setup the wizard's footer has Back.
        let setup = self.mode == FlashMode::Setup;
        d.child(
            div()
                .flex()
                .items_center()
                .justify_between()
                .pt(px(4.))
                .child(div().when(!setup, |d| {
                    d.child(button(t, "back0", "Back", ButtonKind::Small, true, move |_, _, cx| {
                        let _ = me1.update(cx, |v, cx| {
                            v.restart();
                            cx.notify();
                        });
                    }))
                }))
                .child(actions),
        )
    }

    fn firmware_view(&mut self, t: &Theme, cx: &mut Context<Self>) -> gpui::Div {
        let board = self.board_image();
        let env = self.pick.clone().unwrap_or_default();
        let info = boards::board(&env);
        let source = self.source.as_ref().and_then(|s| s.as_ref().ok()).cloned();
        let ver = source.as_ref().map(Source::version).unwrap_or_default();
        let home = self.paths.home.clone();
        // Where it comes from and how that's going sit on separate rows: a
        // local folder path is long, and the state must never be the part
        // that gets cut off.
        let from: SharedString = source.as_ref().map_or_else(|| "–".to_owned(), |s| s.label(&home)).into();
        let state: SharedString = match (&self.image, &source) {
            (None, Some(_)) => format!("Downloading… {:.0}%", self.download * 100.).into(),
            (Some(Ok(_)), Some(_)) => "Checksum verified".into(),
            (Some(Err(_)), Some(_)) => "Failed".into(),
            _ => "–".into(),
        };
        let title = match &self.image {
            None => "Downloading firmware",
            Some(Ok(_)) => "Ready to install",
            Some(Err(_)) => "Couldn't download firmware",
        };
        let mut d = div()
            .flex()
            .flex_col()
            .gap(px(14.))
            .child(div().font_family(FONT_DISPLAY).text_size(px(20.)).child(title))
            .child(
                card(t)
                    .flex_row()
                    .items_center()
                    .gap(px(18.))
                    .child(match info {
                        Some(b) => board_art(b, 132., 110.).into_any_element(),
                        None => clawd(4.).into_any_element(),
                    })
                    .child(kv(
                        t,
                        vec![
                            ("Board", info.map(|b| b.name.to_owned()).unwrap_or(env.clone()).into()),
                            ("Firmware", ver.clone().into()),
                            ("Verification", state),
                        ],
                    )),
            );
        // Where the image comes from and goes to: for diagnosis, not the
        // decision.
        let open = self.details;
        let text = t.text;
        d = d.child(
            div()
                .id("details")
                .group("details")
                .debug_selector(|| "details".into())
                .flex()
                .items_center()
                .gap(px(6.))
                .text_size(px(13.))
                .text_color(t.dim)
                .cursor_pointer()
                .hover(|s| s.text_color(t.text))
                .on_click(cx.listener(|v, _, _, cx| {
                    v.details = !v.details;
                    cx.notify();
                }))
                .child(
                    icon(if open { "chevron-down" } else { "chevron-right" }, 16., t.dim)
                        .group_hover("details", move |s| s.text_color(text)),
                )
                .child("Details"),
        );
        if open {
            d = d.child(div().pl(px(22.)).child(kv(
                t,
                vec![
                    ("File", board.as_ref().map(|b| b.file.clone()).unwrap_or_default().into()),
                    ("Source", from),
                    ("USB", self.port.clone().unwrap_or_default().into()),
                ],
            )));
        }
        if let Some(Err(e)) = &self.image {
            d = d.child(notice(t, Tone::Bad, e.clone()));
        }
        let ready = matches!(self.image, Some(Ok(_)));
        let (me1, me2) = (cx.entity().downgrade(), cx.entity().downgrade());
        let setup = self.mode == FlashMode::Setup;
        d.child(
            div()
                .flex()
                .items_center()
                .justify_between()
                .child(div().when(!setup, |d| {
                    d.child(button(t, "back1", "Back", ButtonKind::Small, true, move |_, _, cx| {
                        let _ = me1.update(cx, |v, cx| {
                            v.back();
                            cx.notify();
                        });
                    }))
                }))
                .child(button(t, "flash", format!("Install {ver}"), ButtonKind::Primary, ready, move |_, _, cx| {
                    let _ = me2.update(cx, |v, cx| v.start_flash(cx));
                })),
        )
    }

    fn flash_view(&mut self, t: &Theme, cx: &mut Context<Self>) -> gpui::Div {
        let title = if self.error.is_some() {
            "Firmware installation failed"
        } else if self.flashing {
            "Writing firmware"
        } else {
            "Firmware installed"
        };
        let mut log = div()
            .id("log")
            .flex()
            .flex_col()
            .h(px(150.))
            .overflow_y_scroll()
            .p(px(12.))
            .rounded(px(R_S))
            .bg(t.bg)
            .border_1()
            .border_color(t.line)
            .font_family(FONT_MONO)
            .text_size(px(12.))
            .text_color(t.dim);
        for l in &self.log {
            log = log.child(l.clone());
        }
        let mut d = div()
            .flex()
            .flex_col()
            .gap(px(12.))
            .child(
                div()
                    .flex()
                    .justify_between()
                    .child(div().font_family(FONT_DISPLAY).text_size(px(20.)).child(title))
                    .child(div().font_family(FONT_MONO).child(format!("{:.0}%", self.progress * 100.))),
            )
            .child(bar(t, self.progress, if self.step == Step::Done { t.ok } else { t.accent }))
            .child(log)
            .child(caption(t, "Keep the USB cable connected until installation finishes."));
        if self.error.is_some() && !self.flashing {
            let (me1, me2) = (cx.entity().downgrade(), cx.entity().downgrade());
            d = d.child(
                div()
                    .flex()
                    .items_center()
                    .justify_between()
                    .child(button(t, "start-over", "Start over", ButtonKind::Small, true, move |_, _, cx| {
                        let _ = me1.update(cx, |v, cx| {
                            v.restart();
                            cx.notify();
                        });
                    }))
                    .child(button(t, "retry", "Retry", ButtonKind::Primary, true, move |_, _, cx| {
                        let _ = me2.update(cx, |v, cx| v.start_flash(cx));
                    })),
            );
        }
        d
    }

    fn done_view(&mut self, t: &Theme, cx: &mut Context<Self>) -> gpui::Div {
        let me = cx.entity().downgrade();
        div()
            .flex()
            .flex_col()
            .items_center()
            .gap(px(12.))
            .pt(px(12.))
            .child(clawd(4.))
            .child(div().font_family(FONT_DISPLAY).text_size(px(22.)).child("Firmware installed"))
            .child(div().max_w(px(440.)).text_center().child(para(
                t,
                if self.connected {
                    "Bluetooth connected."
                } else {
                    "If the board isn't paired yet, choose Clawdmeter in your Bluetooth settings."
                },
            )))
            .when(!self.connected, |d| d.child(pill(t, "Waiting for Bluetooth connection", Tone::Accent)))
            .child(
                div()
                    .flex()
                    .gap(px(8.))
                    .child(button(t, "again", "Flash another board", ButtonKind::Small, true, move |_, _, cx| {
                        let _ = me.update(cx, |v, cx| {
                            v.restart();
                            cx.notify();
                        });
                    }))
                    .child(button(
                        t,
                        "settings",
                        "Device settings",
                        ButtonKind::Small,
                        true,
                        cx.listener(|_, _, _, cx| cx.emit(Navigate(Page::Device))),
                    )),
            )
    }
}

impl Render for FlasherView {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let t = Theme::for_appearance(window.appearance());
        let setup = self.mode == FlashMode::Setup;
        let view = match self.step {
            Step::Connect => self.connect_view(&t, cx),
            Step::Identify => self.identify_view(&t, cx),
            Step::Firmware => self.firmware_view(&t, cx),
            Step::Flash => self.flash_view(&t, cx),
            // The wizard moves on to pairing by itself.
            Step::Done if setup => self.flash_view(&t, cx),
            Step::Done => self.done_view(&t, cx),
        };
        let mut body = div()
            .flex()
            .flex_col()
            .gap(px(18.))
            .when(!setup, |d| d.child(super::widgets::heading(&t, "Flash")).child(self.steps(&t)))
            .child(view);
        if let (Some(e), false) = (&self.error, self.step == Step::Flash) {
            let me = cx.entity().downgrade();
            body = body.child(notice(&t, Tone::Bad, e.clone()).child(div().flex().pl(px(18.)).child(button(
                &t,
                "err-retry",
                "Start over",
                ButtonKind::Small,
                true,
                move |_, _, cx| {
                    let _ = me.update(cx, |v, cx| {
                        v.restart();
                        cx.notify();
                    });
                },
            ))));
        }
        body
    }
}

#[cfg(test)]
pub(crate) mod tests {
    use std::sync::Mutex;

    use gpui::{Modifiers, TestAppContext, VisualTestContext};

    use super::*;

    /// A board on a fake port: an ESP32-S3 with 16 MB that doesn't run
    /// Clawdmeter yet. Records what gets flashed.
    #[derive(Default)]
    pub(crate) struct FakeBoard {
        flashed: Mutex<Option<(String, usize, String)>>,
    }

    impl FlashHardware for FakeBoard {
        fn ports(&self) -> Vec<String> {
            vec!["/dev/fake0".into()]
        }
        fn probe_info(&self, _port: &str) -> Option<FirmwareInfo> {
            None
        }
        fn probe_chip(&self, _port: &str) -> Result<ChipInfo, DeviceError> {
            Ok(ChipInfo { chip: "esp32s3".into(), flash_mb: Some(16), mac: None, features: vec![] })
        }
        fn flash(
            &self,
            port: &str,
            image: &[u8],
            board: &BoardImage,
            progress: &mut dyn FnMut(FlashProgress),
        ) -> Result<(), DeviceError> {
            progress(FlashProgress::Start { total: image.len() });
            progress(FlashProgress::Write { done: image.len() / 2 });
            progress(FlashProgress::Write { done: image.len() });
            progress(FlashProgress::Verify);
            progress(FlashProgress::Done);
            *self.flashed.lock().unwrap() = Some((port.into(), image.len(), board.env.clone()));
            Ok(())
        }
    }

    fn release_dir(tag: &str) -> (std::path::PathBuf, Paths) {
        let root = std::env::temp_dir().join(format!("cm-flasher-{tag}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        let rel = root.join("release");
        std::fs::create_dir_all(&rel).unwrap();
        let image = vec![0xA5u8; 4096];
        std::fs::write(rel.join("waveshare_amoled_18-v1.9.0.bin"), &image).unwrap();
        let sha = release::sha256_hex(&image);
        let manifest = format!(
            r#"{{"version":"1.9.0","boards":[
              {{"env":"waveshare_amoled_216","chip":"esp32s3","flash_size":"8MB","display_name":"2.16","file":"waveshare_amoled_216-v1.9.0.bin","sha256":"00"}},
              {{"env":"waveshare_amoled_18","chip":"esp32s3","flash_size":"16MB","display_name":"1.8","file":"waveshare_amoled_18-v1.9.0.bin","sha256":"{sha}"}},
              {{"env":"waveshare_amoled_216_c6","chip":"esp32c6","flash_size":"16MB","display_name":"C6","file":"c6.bin","sha256":"00"}}]}}"#
        );
        std::fs::write(rel.join("manifest.json"), manifest).unwrap();
        (rel.join("manifest.json"), Paths::under(&root))
    }

    fn click(cx: &mut VisualTestContext, selector: &'static str) {
        cx.run_until_parked();
        let b = cx.debug_bounds(selector).unwrap_or_else(|| panic!("{selector} isn't on screen"));
        cx.simulate_click(b.center(), Modifiers::none());
        cx.run_until_parked();
    }

    #[gpui::test]
    fn walks_from_plug_in_to_done(cx: &mut TestAppContext) {
        let (manifest, paths) = release_dir("flow");
        let hw = Arc::new(FakeBoard::default());
        let hw2: Arc<dyn FlashHardware> = hw.clone();
        let p = paths.clone();
        let (view, cx) = cx.add_window_view(move |_, cx| {
            let mut v = FlasherView::with_hardware(p, Some(manifest.clone()), hw2, cx);
            v.set_active(true, cx);
            v
        });

        // Connect → Identify: the tick notices the port and probes the chip.
        cx.executor().advance_clock(REFRESH * 3);
        cx.run_until_parked();
        view.read_with(cx, |v, _| {
            assert!(v.step == Step::Identify);
            assert_eq!(v.port.as_deref(), Some("/dev/fake0"));
            // 16 MB S3: the exact match first, the 8 MB 2.16 image still offered.
            assert_eq!(v.candidates, ["waveshare_amoled_18", "waveshare_amoled_216"]);
            assert!(v.pick.is_none());
        });

        // Continue stays disabled until a board is picked.
        click(cx, "continue");
        view.read_with(cx, |v, _| assert!(v.step == Step::Identify));
        click(cx, "board-0");
        click(cx, "continue");
        view.read_with(cx, |v, _| {
            assert!(v.step == Step::Firmware);
            assert!(matches!(v.image, Some(Ok(ref d)) if d.len() == 4096), "local image read and checksum-verified");
        });

        click(cx, "flash");
        cx.run_until_parked();
        view.read_with(cx, |v, _| {
            assert!(v.step == Step::Done, "log: {:?}, error: {:?}", v.log, v.error);
            assert_eq!(v.progress, 1.);
        });
        assert_eq!(
            hw.flashed.lock().unwrap().clone(),
            Some(("/dev/fake0".to_string(), 4096, "waveshare_amoled_18".to_string()))
        );
        let rec = ipc::load_firmware_record(&paths).unwrap();
        assert_eq!((rec.env.as_str(), rec.ver.as_str()), ("waveshare_amoled_18", "1.9.0"));
        let _ = std::fs::remove_dir_all(&paths.home);
    }

    #[gpui::test]
    fn missing_image_is_reported_and_blocks_flashing(cx: &mut TestAppContext) {
        let (manifest, paths) = release_dir("missing");
        let hw: Arc<dyn FlashHardware> = Arc::new(FakeBoard::default());
        let p = paths.clone();
        let (view, cx) = cx.add_window_view(move |_, cx| {
            let mut v = FlasherView::with_hardware(p, Some(manifest.clone()), hw, cx);
            v.set_active(true, cx);
            v
        });
        cx.executor().advance_clock(REFRESH * 3);
        click(cx, "board-1");
        click(cx, "continue");
        view.read_with(cx, |v, _| match &v.image {
            Some(Err(e)) => assert_eq!(e, "Place waveshare_amoled_216-v1.9.0.bin in the same folder as manifest.json."),
            other => {
                panic!("expected a missing-file error, got {:?}", other.as_ref().map(|r| r.as_ref().map(Vec::len)))
            }
        });
        click(cx, "flash");
        view.read_with(cx, |v, _| assert!(v.step == Step::Firmware, "disabled Flash does nothing"));
        let _ = std::fs::remove_dir_all(&paths.home);
    }
}
