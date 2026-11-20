//! Send a firmware image to the paired Clawdmeter over Bluetooth without the
//! app, for trying updates on hardware:
//!
//! ```text
//! cargo run -p clawdmeter-ble --example ota -- <image.bin> [env] [--with-response]
//! ```
//!
//! `--with-response` sends every data frame with response, the path a
//! stalled transfer falls back to.
//!
//! `image.bin` is PlatformIO's application image
//! (`firmware/.pio/build/<env>/firmware.bin`) or a release's merged image
//! (its app partition is cut out). `env` defaults to the one the board
//! reports. Quit the Clawdmeter app first so nothing else writes to the
//! board meanwhile. Ctrl-C cancels (the board keeps its firmware).

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::Instant;

use clawdmeter_ble::{AddressCache, Bluetooth, DeviceLink, LinkOptions, UpdateProgress};

/// A merged image's application part (partition table at 0x8000, first
/// `app` entry), or the file itself when it has no partition table.
fn app_part(image: &[u8]) -> &[u8] {
    let Some(table) = image.get(0x8000..0x8C00) else { return image };
    let off = table
        .as_chunks::<32>()
        .0
        .iter()
        .take_while(|e| e[..2] == [0xAA, 0x50])
        .find_map(|e| (e[2] == 0x00).then(|| u32::from_le_bytes([e[4], e[5], e[6], e[7]]) as usize));
    match off.and_then(|o| image.get(o..)) {
        Some(app) => app,
        None => image,
    }
}

#[tokio::main(flavor = "current_thread")]
async fn main() {
    let with_response = std::env::args().any(|a| a == "--with-response");
    let mut args = std::env::args().skip(1).filter(|a| a != "--with-response");
    let Some(path) = args.next() else {
        eprintln!("usage: ota <image.bin> [env]");
        std::process::exit(2);
    };
    let file = std::fs::read(&path).unwrap_or_else(|e| {
        eprintln!("{path}: {e}");
        std::process::exit(1);
    });
    let image = app_part(&file).to_vec();
    if image.first() != Some(&0xE9) {
        eprintln!("{path} isn't an ESP application image");
        std::process::exit(1);
    }

    let bt = Bluetooth::default();
    let cache = AddressCache::new(std::env::temp_dir().join("clawdmeter-ota-example-address"));
    let (link, _events) = DeviceLink::open(&bt, &cache, &LinkOptions::default()).await.unwrap_or_else(|e| {
        eprintln!("couldn't connect: {e}");
        std::process::exit(1);
    });
    let Some(info) = link.info().cloned() else {
        eprintln!("the board's firmware predates Bluetooth updates; flash it over USB once");
        std::process::exit(1);
    };
    println!("board: {} {} (update slot {} bytes)", info.env, info.ver, info.ota_slot);
    let Some(mut updater) = link.updater() else {
        eprintln!("this board can't take updates over Bluetooth");
        std::process::exit(1);
    };
    if with_response {
        updater = updater.with_response();
    }
    let env = args.next().unwrap_or(info.env);

    let cancel = Arc::new(AtomicBool::new(false));
    let c = cancel.clone();
    tokio::spawn(async move {
        let _ = tokio::signal::ctrl_c().await;
        c.store(true, Ordering::Relaxed);
    });
    let start = Instant::now();
    let mut shown = u32::MAX;
    let r = updater
        .run(
            &image,
            &env,
            |p| match p {
                UpdateProgress::Starting => println!("starting ({} bytes for {env})", image.len()),
                UpdateProgress::Confirming { secs } => {
                    println!("allow it on the board within {secs} s: tap its screen or press a button twice")
                }
                UpdateProgress::Sending { done, total } => {
                    let pct = (u64::from(done) * 100 / u64::from(total.max(1))) as u32;
                    if pct != shown && pct.is_multiple_of(5) {
                        shown = pct;
                        let secs = start.elapsed().as_secs_f32().max(0.1);
                        println!("{pct:3}%  {done}/{total} bytes  {:.1} KB/s", done as f32 / 1024. / secs);
                    }
                }
                UpdateProgress::Verifying => println!("checking on the board"),
            },
            &cancel,
        )
        .await;
    match r {
        Ok(()) => println!("done in {:.0} s; the board restarts into it", start.elapsed().as_secs_f32()),
        Err(e) => {
            eprintln!("{e}");
            std::process::exit(1);
        }
    }
}
