//! Firmware delivery: the release manifest, download + sha256, board
//! detection over USB, and writing the merged image at 0x0 with espflash.
//! Nobody needs PlatformIO or Python installed.
//!
//! Typical flow (blocking; run it off the UI thread):
//!
//! ```no_run
//! use clawdmeter_flash::{device, release};
//! let rel = release::release_for_app("HermannBjorgvin/Clawdmeter", "2.0.0")?;
//! let port = &device::list_ports()[0].path;
//! let env = match device::identify(port, &rel.manifest)? {
//!     device::Identification::Firmware(info) => info.env,
//!     device::Identification::Unique(env) => env,
//!     device::Identification::Choose(envs) => envs[0].clone(), // ask the user
//!     device::Identification::Unsupported(_) => panic!("unsupported board"),
//! };
//! let board = rel.manifest.board(&env).unwrap();
//! let image = release::fetch_image(&rel, board, std::path::Path::new("/tmp/cache"), |_, _| {})?;
//! device::flash_merged(port, &image, board, |_| {})?;
//! # Ok::<(), Box<dyn std::error::Error>>(())
//! ```

#![warn(missing_docs)]

pub mod device;
pub mod manifest;
pub mod release;

pub use device::{FirmwareInfo, Identification};
pub use manifest::{BoardImage, Manifest};
pub use release::{Release, UpdateCheck};

/// Default release repository.
pub use clawdmeter_core::config::DEFAULT_RELEASE_REPO;
