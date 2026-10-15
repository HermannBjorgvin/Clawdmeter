//! `clawdmeter`: the desktop companion.
//!
//! - no arguments: the tray app (daemon + tray icon; the app window runs as
//!   a `--window` child process).
//! - `--daemon`: headless (no window, no tray) for servers and CI.
//! - `--window [PAGE]`: the app window (one per user; a second call switches
//!   the open one to PAGE).

// The tray app is a GUI program on Windows: no console window. CLI modes
// attach to the parent console instead (see `attach_console`).
#![cfg_attr(windows, windows_subsystem = "windows")]

use clawdmeter_app::{daemon, logger, tray, ui, Page};
use clawdmeter_core::Paths;

const USAGE: &str = "\
clawdmeter - Claude Code usage on your desk

USAGE:
    clawdmeter [OPTIONS]

With no options, runs the tray app: usage updates plus a tray icon (or the app
window on desktops without a tray).

OPTIONS:
    --daemon          Run headless: poll usage, follow hooks, send updates over Bluetooth LE
    --window [PAGE]   Open the app window on a page: overview (default), device,
                      flash, accounts, hooks, settings or setup
      [--manifest P]  flash page: use a local release manifest.json instead of GitHub
    --once            Poll /usage for every account once, print the payload, exit (no BLE)
    --ports           List attached Espressif USB ports
    --info PORT       Ask the firmware on PORT for its env/version
    hook MODE [event] Claude Code hook (MODE: alert, clear or event); reads the
                      hook JSON on stdin, prints nothing, always exits 0
    -V, --version
    -h, --help

ENVIRONMENT:
    CLAWDMETER_LOG       error|warn|info|debug (default info)
    CLAWDMETER_NO_TRAY   1 = show the app window instead of a tray icon
    POLL_INTERVAL        default poll interval (the config file overrides it)
";

#[cfg(windows)]
fn attach_console() {
    use windows_sys::Win32::System::Console::{AttachConsole, ATTACH_PARENT_PROCESS};
    // SAFETY: plain Win32 call; failure (no parent console) is fine.
    unsafe {
        AttachConsole(ATTACH_PARENT_PROCESS);
    }
}

#[cfg(not(windows))]
fn attach_console() {}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    // Claude Code runs this on every hook event: no console, no logger, no
    // GPUI, no tray; read stdin, write a file, exit 0.
    if args.first().map(String::as_str) == Some("hook") {
        std::process::exit(clawdmeter_app::hooks::main(&args[1..]));
    }
    let paths = Paths::for_user();
    if args.is_empty() {
        logger::init_with_file(&paths.state_dir.join("daemon.log"));
        warn_if_legacy_daemon(&paths);
        std::process::exit(tray::run(paths));
    }
    attach_console();
    logger::init();
    let code = match args[0].as_str() {
        "--daemon" => run_daemon(paths),
        "--window" => {
            let opt = |name: &str| args.iter().position(|a| a == name).and_then(|i| args.get(i + 1)).cloned();
            // `--tab` is the old settings-window flag; it names a page too.
            let name = opt("--tab").or_else(|| args.get(1).filter(|a| !a.starts_with("--")).cloned());
            match name.as_deref().map(Page::parse) {
                Some(None) => {
                    eprintln!("--window takes one of: overview, device, flash, accounts, hooks, settings, setup");
                    2
                }
                page => ui::run(page.flatten(), paths, ui::WindowArgs { manifest: opt("--manifest").map(Into::into) }),
            }
        }
        "--once" => daemon::once(&paths),
        "--ports" => {
            for p in clawdmeter_flash::device::list_ports() {
                println!("{}  {:04x}:{:04x}  {}", p.path, p.vid, p.pid, p.serial_number.unwrap_or_default());
            }
            0
        }
        "--info" => match args.get(1) {
            Some(port) => match clawdmeter_flash::device::probe_info(port, std::time::Duration::from_secs(2)) {
                Ok(Some(i)) => {
                    println!("env={} ver={} rev={} link={}", i.env, i.ver, i.rev, i.link);
                    0
                }
                Ok(None) => {
                    eprintln!("No firmware reply. Check USB and whether the board supports the info command.");
                    1
                }
                Err(e) => {
                    eprintln!("{e}");
                    1
                }
            },
            None => {
                eprintln!("--info needs a port");
                2
            }
        },
        "-V" | "--version" => {
            println!("clawdmeter {}", clawdmeter_app::version());
            0
        }
        "-h" | "--help" => {
            print!("{USAGE}");
            0
        }
        other => {
            eprintln!("unknown option {other}\n\n{USAGE}");
            2
        }
    };
    std::process::exit(code);
}

/// The bash / Python daemon this app replaces would fight it for the device.
/// The Settings page turns it off (`autostart::disable_legacy`).
fn warn_if_legacy_daemon(paths: &Paths) {
    if let Some(e) = clawdmeter_core::autostart::legacy_entry(&paths.home) {
        log::warn!(
            "A legacy Clawdmeter service is enabled ({}). Disable it in Settings so only one process connects to the \
             device.",
            e.describe()
        );
    }
}

fn run_daemon(paths: Paths) -> i32 {
    warn_if_legacy_daemon(&paths);
    let _lock = match clawdmeter_app::ipc::acquire_instance_lock(&paths) {
        Ok(Some(f)) => Some(f),
        Ok(None) => {
            eprintln!("Clawdmeter is already running (tray app or --daemon); not starting a second daemon");
            return 1;
        }
        Err(e) => {
            log::warn!("instance lock unavailable ({e}); continuing");
            None
        }
    };
    let rt = match tokio::runtime::Builder::new_current_thread().enable_all().build() {
        Ok(rt) => rt,
        Err(e) => {
            eprintln!("runtime: {e}");
            return 1;
        }
    };
    rt.block_on(async move {
        log::info!("=== Clawdmeter daemon {} ===", clawdmeter_app::version());
        let (cmd_tx, cmd_rx) = tokio::sync::mpsc::unbounded_channel();
        let (st_tx, _st_rx) = tokio::sync::watch::channel(daemon::Status::default());
        tokio::spawn(async move {
            shutdown_signal().await;
            let _ = cmd_tx.send(daemon::Command::Shutdown);
        });
        daemon::run(paths, cmd_rx, st_tx).await;
    });
    0
}

async fn shutdown_signal() {
    #[cfg(unix)]
    {
        use tokio::signal::unix::{signal, SignalKind};
        let mut term = match signal(SignalKind::terminate()) {
            Ok(s) => s,
            Err(_) => {
                let _ = tokio::signal::ctrl_c().await;
                return;
            }
        };
        tokio::select! {
            _ = tokio::signal::ctrl_c() => {}
            _ = term.recv() => {}
        }
    }
    #[cfg(not(unix))]
    {
        let _ = tokio::signal::ctrl_c().await;
    }
}
