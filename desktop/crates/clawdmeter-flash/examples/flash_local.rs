//! Flash a board from a local release folder, through the same code path as
//! the app's flasher, without opening a window.
//!
//!     cargo run -p clawdmeter-flash --example flash_local -- <dist-dir> <env> <port>
//!
//! `<dist-dir>` holds `manifest.json` and the merged images
//! (`tools/ci/merge_firmware.py` + `make_manifest.py` produce it). After the
//! write the board is reset once and must come back answering `info` as
//! `<env>`; the reported version is printed. The flasher's debug log goes to
//! stderr with the time since start, so a slow boot confirmation shows each
//! `info` attempt and why it failed (port missing, busy, no answer).

use std::path::PathBuf;
use std::time::Instant;
use std::{env, fs, process};

use clawdmeter_flash::device::{self, FlashProgress};
use clawdmeter_flash::release;
use clawdmeter_flash::Manifest;

/// `clawdmeter_flash` debug lines on stderr, stamped with the time since start.
struct StderrLog(Instant);

impl log::Log for StderrLog {
    fn enabled(&self, m: &log::Metadata) -> bool {
        m.target().starts_with("clawdmeter_flash") || m.level() <= log::Level::Warn
    }
    fn log(&self, r: &log::Record) {
        if self.enabled(r.metadata()) {
            eprintln!("[{:7.2}s] {}", self.0.elapsed().as_secs_f32(), r.args());
        }
    }
    fn flush(&self) {}
}

fn main() {
    let logger = Box::leak(Box::new(StderrLog(Instant::now())));
    if log::set_logger(logger).is_ok() {
        log::set_max_level(log::LevelFilter::Debug);
    }
    let args: Vec<String> = env::args().skip(1).collect();
    let [dist, env_name, port] = args.as_slice() else {
        eprintln!("usage: flash_local <dist-dir> <env> <port>");
        process::exit(2);
    };
    if let Err(e) = run(PathBuf::from(dist), env_name, port) {
        eprintln!("error: {e}");
        process::exit(1);
    }
}

fn run(dist: PathBuf, env_name: &str, port: &str) -> Result<(), Box<dyn std::error::Error>> {
    let manifest = Manifest::parse(&fs::read_to_string(dist.join("manifest.json"))?)?;
    let board = manifest.board(env_name).ok_or_else(|| format!("{env_name} is not in the manifest"))?;
    let image = fs::read(dist.join(&board.file))?;
    release::verify(board, &image)?;
    println!("{} {} verified ({} bytes)", board.file, manifest.version, image.len());

    let mut total = 0;
    let mut last_pct = usize::MAX;
    let mut reset_at = None;
    let info = device::flash_and_confirm(port, &image, board, |p| match p {
        FlashProgress::Start { total: t } => total = t.max(1),
        FlashProgress::Write { done } => {
            let pct = done * 100 / total.max(1);
            if pct / 10 != last_pct / 10 {
                println!("  {pct}%");
                last_pct = pct;
            }
        }
        FlashProgress::Verify => println!("  verifying"),
        FlashProgress::Done => {
            reset_at = Some(Instant::now());
            println!("  done, resetting; waiting for the board to answer `info`")
        }
    });
    let waited = reset_at.map_or(0.0, |t| t.elapsed().as_secs_f32());
    let info = info.map_err(|e| format!("{e} (after {waited:.1} s)"))?;
    println!("answered `info` {waited:.1} s after the reset");
    println!("running: {} {} (rev {:?}, link {:?})", info.env, info.ver, info.rev, info.link);
    if info.ver.trim_start_matches('v') != manifest.version.trim_start_matches('v') {
        println!("warning: the board reports version {}, the manifest says {}", info.ver, manifest.version);
    }
    Ok(())
}
