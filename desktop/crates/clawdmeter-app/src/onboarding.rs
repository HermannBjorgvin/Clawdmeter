//! First-run setup state: `onboarding_done = true` in the config file once
//! the setup wizard is finished or skipped.
//!
//! Installs that already know their device (a cached BLE address from the
//! old daemons or an earlier version) count as set up, so upgrading never
//! puts anyone through the wizard.

use std::io;

use clawdmeter_core::config::ConfigFile;
use clawdmeter_core::Paths;

/// The config key.
pub const KEY: &str = "onboarding_done";

fn truthy(v: &str) -> bool {
    matches!(v.to_ascii_lowercase().as_str(), "true" | "on" | "1" | "yes")
}

/// Should the setup wizard open on its own?
pub fn needed(paths: &Paths) -> bool {
    let done = ConfigFile::load(&paths.config_file).ok().and_then(|c| c.get(KEY).map(truthy)).unwrap_or(false);
    !done && !paths.ble_address_file().exists()
}

/// Record that setup was finished or skipped (comments and other keys are
/// kept).
pub fn mark_done(paths: &Paths) -> io::Result<()> {
    let mut cfg = ConfigFile::load(&paths.config_file)?;
    if cfg.get(KEY).is_some_and(truthy) {
        return Ok(());
    }
    cfg.set(KEY, "true");
    cfg.save(&paths.config_file)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn done_once_marked_or_a_device_is_known() {
        let root = std::env::temp_dir().join(format!("cm-onboarding-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        let paths = Paths::under(&root);
        assert!(needed(&paths));
        std::fs::create_dir_all(&paths.state_dir).unwrap();
        std::fs::write(&paths.config_file, "# mine\nbeep = on\n").unwrap();
        mark_done(&paths).unwrap();
        assert!(!needed(&paths));
        let text = std::fs::read_to_string(&paths.config_file).unwrap();
        assert_eq!(text, "# mine\nbeep = on\nonboarding_done = true\n");

        std::fs::write(&paths.config_file, "").unwrap();
        assert!(needed(&paths));
        std::fs::write(paths.ble_address_file(), "AA:BB").unwrap();
        assert!(!needed(&paths), "an existing install skips the wizard");
        let _ = std::fs::remove_dir_all(&root);
    }
}
