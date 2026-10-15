//! The app window with fake data and a fake board, for screenshots and UI
//! work without hardware or a running daemon. Point HOME (and
//! XDG_CACHE_HOME) at a scratch dir: it writes a status file, a config and
//! a release folder there.
//!
//! ```text
//! HOME=/tmp/cm XDG_CACHE_HOME=/tmp/cm/.cache \
//!   cargo run -p clawdmeter-app --example demo -- [--page PAGE] [--status connected|off]
//!   [--board none|fresh|current|old] [--setup welcome|board|flash|pair|hooks|done] [--hooks-preview]
//!   [--flash-to identify|firmware|flash|done] [--shop] [--accounts retrying] [--refresh wait|checking]
//!   [--advanced] [--session idle] [--height PX]
//! ```
//!
//! `--accounts retrying` adds a second account that is being rate-limited;
//! `--refresh` shows the Overview's refresh button waiting out the
//! per-account gap, or mid-check.

use std::sync::Arc;
use std::time::Duration;

use clawdmeter_app::ipc::{self, FirmwareRecord, StatusSnapshot};
use clawdmeter_app::ui::app::AppView;
use clawdmeter_app::ui::flasher::{FlashHardware, FlasherView, Step as FlashStep};
use clawdmeter_app::ui::overview::OverviewView;
use clawdmeter_app::ui::settings::SettingsView;
use clawdmeter_app::ui::setup::Step as SetupStep;
use clawdmeter_app::Page;
use clawdmeter_core::{unix_now, Paths};
use clawdmeter_flash::device::{ChipInfo, DeviceError, FirmwareInfo, FlashProgress};
use clawdmeter_flash::release::sha256_hex;
use clawdmeter_flash::BoardImage;
use gpui::AppContext as _;

const VERSION: &str = "1.9.0";
const ENV: &str = "waveshare_amoled_216";

/// A board on a fake port that takes its time.
struct FakeBoard {
    kind: String,
}

impl FlashHardware for FakeBoard {
    fn ports(&self) -> Vec<String> {
        if self.kind == "none" {
            vec![]
        } else {
            vec!["/dev/ttyACM0".into()]
        }
    }
    fn probe_info(&self, _port: &str) -> Option<FirmwareInfo> {
        std::thread::sleep(Duration::from_millis(600));
        let ver = match self.kind.as_str() {
            "current" => VERSION,
            "old" => "1.8.2",
            _ => return None,
        };
        Some(FirmwareInfo { env: ENV.into(), ver: ver.into(), rev: String::new(), link: String::new() })
    }
    fn probe_chip(&self, _port: &str) -> Result<ChipInfo, DeviceError> {
        std::thread::sleep(Duration::from_millis(600));
        Ok(ChipInfo { chip: "esp32s3".into(), flash_mb: Some(16), mac: None, features: vec!["PSRAM".into()] })
    }
    fn flash(
        &self,
        _port: &str,
        image: &[u8],
        _board: &BoardImage,
        progress: &mut dyn FnMut(FlashProgress),
    ) -> Result<(), DeviceError> {
        progress(FlashProgress::Start { total: image.len() });
        for i in 1..=200 {
            std::thread::sleep(Duration::from_millis(100));
            progress(FlashProgress::Write { done: image.len() * i / 200 });
        }
        progress(FlashProgress::Verify);
        progress(FlashProgress::Done);
        Ok(())
    }
}

fn arg(args: &[String], name: &str) -> Option<String> {
    args.iter().position(|a| a == name).and_then(|i| args.get(i + 1)).cloned()
}

/// A local release with every board, so the picker and flasher have
/// something to show. Only the 2.16's image exists.
fn release(paths: &Paths) -> std::path::PathBuf {
    let dir = paths.cache_dir.join("demo-release");
    std::fs::create_dir_all(&dir).unwrap();
    let image = vec![0xA5u8; 3 * 1024 * 1024 + 300 * 1024];
    let file = format!("{ENV}-v{VERSION}.bin");
    std::fs::write(dir.join(&file), &image).unwrap();
    let sha = sha256_hex(&image);
    let boards: Vec<String> = clawdmeter_app::boards::BOARDS
        .iter()
        .map(|b| {
            let chip = if b.chip.contains("C6") { "esp32c6" } else { "esp32s3" };
            let flash = if b.env == "waveshare_amoled_206" { "32MB" } else { "16MB" };
            let (f, s) = if b.env == ENV { (file.clone(), sha.clone()) } else { (format!("{}.bin", b.env), "00".into()) };
            format!(
                r#"{{"env":"{}","chip":"{chip}","flash_size":"{flash}","display_name":"{}","file":"{f}","sha256":"{s}"}}"#,
                b.env, b.product
            )
        })
        .collect();
    let manifest = format!(r#"{{"version":"{VERSION}","boards":[{}]}}"#, boards.join(","));
    std::fs::write(dir.join("manifest.json"), manifest).unwrap();
    dir.join("manifest.json")
}

fn fake_status(paths: &Paths, retrying: bool, refresh: Option<String>, idle: bool) {
    let paths = paths.clone();
    let work = paths.home.join(".claude-work");
    if retrying {
        std::fs::create_dir_all(&work).unwrap();
        std::fs::write(&paths.config_file, "config_dirs = ~/.claude, ~/.claude-work\n").unwrap();
    }
    let started = unix_now();
    std::thread::spawn(move || loop {
        let now = unix_now();
        let errors = if retrying {
            vec![ipc::AccountError {
                dir: work.display().to_string(),
                message: "Usage requests are temporarily limited. Trying again in about 10 minutes.".into(),
                retrying: true,
            }]
        } else {
            vec![]
        };
        let snap = StatusSnapshot {
            updated_at: now,
            link: "connected".into(),
            device_key: Some("AA:BB:CC:DD:EE:FF".into()),
            device_name: Some("Clawdmeter".into()),
            account: Some(paths.home.join(".claude").display().to_string()),
            claude: Some(paths.home.join(".local/bin/claude").display().to_string()),
            session_pct: Some(if idle { 0 } else { 34 }),
            week_pct: Some(62),
            window_started: !idle,
            session_resets_at: (!idle).then_some(started + 177 * 60),
            week_resets_at: Some(started + (5 * 24 + 11) * 3600 + 30),
            read_at: Some(now - 15),
            hooks: true,
            last_write_at: Some(now - 5),
            latest_firmware: Some(VERSION.into()),
            device_firmware: Some("1.8.2".into()),
            tray: "Connected".into(),
            errors,
            refresh_at: (refresh.as_deref() == Some("wait")).then_some(started + 100),
            polling: refresh.as_deref() == Some("checking"),
            ..Default::default()
        };
        let _ = ipc::write_status(&paths, &snap);
        std::thread::sleep(Duration::from_secs(2));
    });
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let paths = Paths::for_user();
    std::fs::create_dir_all(paths.home.join(".claude")).unwrap();
    std::fs::create_dir_all(&paths.state_dir).unwrap();
    if arg(&args, "--status").as_deref() != Some("off") {
        fake_status(
            &paths,
            arg(&args, "--accounts").as_deref() == Some("retrying"),
            arg(&args, "--refresh"),
            arg(&args, "--session").as_deref() == Some("idle"),
        );
        let _ = ipc::save_firmware_record(&paths, &FirmwareRecord { env: ENV.into(), ver: "1.8.2".into() });
    } else {
        let _ = std::fs::remove_file(ipc::status_file(&paths));
    }
    let manifest = release(&paths);
    let page = arg(&args, "--page").and_then(|p| Page::parse(&p)).unwrap_or(Page::Overview);
    let setup_step = arg(&args, "--setup");
    let page = if setup_step.is_some() { Page::Setup } else { page };
    let board = arg(&args, "--board").unwrap_or_else(|| "fresh".into());
    let flash_to = arg(&args, "--flash-to");
    let hooks_preview = args.iter().any(|a| a == "--hooks-preview");
    let shop = args.iter().any(|a| a == "--shop");
    let advanced = args.iter().any(|a| a == "--advanced");
    // A taller window, so a long page fits one screenshot.
    let height = arg(&args, "--height").and_then(|h| h.parse::<f32>().ok());

    clawdmeter_app::ui::open_with_height(height, move |window, cx| {
        let p = paths.clone();
        let hw: Arc<dyn FlashHardware> = Arc::new(FakeBoard { kind: board });
        let overview = cx.new(|cx| OverviewView::new(p.clone(), cx));
        let settings = cx.new(|cx| SettingsView::with_usb(p.clone(), Page::Device, false, cx));
        let flasher = cx.new(|cx| FlasherView::with_hardware(p.clone(), Some(manifest), hw, cx));
        let app = AppView::build(p, page, overview, settings.clone(), flasher.clone(), window, cx);
        if shop {
            flasher.update(cx, |f, cx| f.show_shop(true, cx));
        }
        if advanced {
            settings.update(cx, |s, cx| s.show_advanced(true, cx));
        }
        if hooks_preview {
            settings.update(cx, |s, cx| s.review_hook_install(cx));
        }
        let wizard = app.wizard().clone();
        if let Some(step) = setup_step {
            let step = match step.as_str() {
                "board" | "flash" => SetupStep::Device,
                "pair" => SetupStep::Pair,
                "hooks" => SetupStep::Hooks,
                "done" => SetupStep::Done,
                _ => SetupStep::Welcome,
            };
            wizard.update(cx, |w, cx| w.goto(step, cx));
        }
        // Walk the flasher forward to the requested step.
        if let Some(target) = flash_to {
            cx.spawn(async move |_, cx| loop {
                cx.background_executor().timer(Duration::from_millis(300)).await;
                let done = flasher.update(cx, |f, cx| {
                    let at = f.step();
                    match (target.as_str(), at) {
                        ("identify", FlashStep::Identify) => true,
                        (_, FlashStep::Identify) if !f.candidates().is_empty() => {
                            f.choose(ENV, cx);
                            false
                        }
                        ("firmware", FlashStep::Firmware) => f.image_ready(),
                        (_, FlashStep::Firmware) if f.image_ready() => {
                            f.flash(cx);
                            false
                        }
                        ("flash", FlashStep::Flash) => true,
                        ("done", FlashStep::Done) => true,
                        _ => false,
                    }
                });
                if done.unwrap_or(true) {
                    break;
                }
            })
            .detach();
        }
        app
    });
}
