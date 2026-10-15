//! A tiny stderr logger (`[HH:MM:SS] message`), so the
//! binary doesn't carry env_logger's regex engine. Level from `CLAWDMETER_LOG`
//! (`error|warn|info|debug|trace`, default `info`).

use log::{Level, LevelFilter, Log, Metadata, Record};

struct Stderr {
    level: LevelFilter,
    file: Option<std::sync::Mutex<std::fs::File>>,
}

impl Log for Stderr {
    fn enabled(&self, m: &Metadata) -> bool {
        m.level() <= self.level && (m.target().starts_with("clawdmeter") || m.level() <= Level::Warn)
    }

    fn log(&self, r: &Record) {
        if !self.enabled(r.metadata()) {
            return;
        }
        let t = chrono::Local::now().format("%H:%M:%S");
        let lvl = match r.level() {
            Level::Error => "error: ",
            Level::Warn => "warning: ",
            _ => "",
        };
        let line = format!("[{t}] {lvl}{}", r.args());
        eprintln!("{line}");
        if let Some(f) = &self.file {
            if let Ok(mut f) = f.lock() {
                use std::io::Write as _;
                let _ = writeln!(f, "{line}");
            }
        }
    }

    fn flush(&self) {}
}

/// Install the logger once (stderr only).
pub fn init() {
    install(None);
}

/// Log to stderr and to `path` as well (the tray app has no terminal). The
/// previous log is kept as `<path>.1`.
pub fn init_with_file(path: &std::path::Path) {
    if let Some(dir) = path.parent() {
        let _ = std::fs::create_dir_all(dir);
    }
    let _ = std::fs::rename(path, path.with_extension("log.1"));
    install(std::fs::File::create(path).ok());
}

fn install(file: Option<std::fs::File>) {
    let level = match std::env::var("CLAWDMETER_LOG").unwrap_or_default().to_ascii_lowercase().as_str() {
        "error" => LevelFilter::Error,
        "warn" => LevelFilter::Warn,
        "debug" => LevelFilter::Debug,
        "trace" => LevelFilter::Trace,
        "off" => LevelFilter::Off,
        _ => LevelFilter::Info,
    };
    let logger: &'static Stderr = Box::leak(Box::new(Stderr { level, file: file.map(std::sync::Mutex::new) }));
    if log::set_logger(logger).is_ok() {
        log::set_max_level(level);
    }
}
