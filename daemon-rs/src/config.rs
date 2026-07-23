use std::collections::HashMap;
use std::path::PathBuf;

/// The daemon's config file is a flat `key = value` format (see
/// `daemon/config.example`) — NOT YAML/TOML — re-read on every poll cycle so
/// edits take effect without a restart. This mirrors the Python daemon's line
/// parser (`read_config_dirs`, `read_token_for`, etc.) rather than pulling in
/// a config-file crate for a format this simple.
pub struct Config {
    values: HashMap<String, String>,
}

fn config_path() -> PathBuf {
    if let Ok(dir) = std::env::var("CLAWDMETER_CONFIG_DIR") {
        return PathBuf::from(dir).join("config");
    }
    #[cfg(target_os = "windows")]
    {
        let base = std::env::var("LOCALAPPDATA").unwrap_or_default();
        PathBuf::from(base).join("Clawdmeter").join("config")
    }
    #[cfg(not(target_os = "windows"))]
    {
        dirs::home_dir()
            .unwrap_or_default()
            .join(".config")
            .join("claude-usage-monitor")
            .join("config")
    }
}

impl Config {
    pub fn load() -> Self {
        let mut values = HashMap::new();
        if let Ok(text) = std::fs::read_to_string(config_path()) {
            for line in text.lines() {
                let line = line.split('#').next().unwrap_or("").trim();
                if line.is_empty() {
                    continue;
                }
                if let Some((key, val)) = line.split_once('=') {
                    values.insert(key.trim().to_string(), val.trim().to_string());
                }
            }
        }
        Config { values }
    }

    fn get(&self, key: &str) -> Option<&str> {
        self.values.get(key).map(String::as_str)
    }

    fn comma_list(&self, key: &str, default: &[&str]) -> Vec<String> {
        match self.get(key) {
            Some(raw) if !raw.is_empty() => {
                raw.split(',').map(|s| s.trim().to_string()).filter(|s| !s.is_empty()).collect()
            }
            _ => default.iter().map(|s| s.to_string()).collect(),
        }
    }

    /// Which provider adapters to enable, in the order given. Defaults to
    /// `["claude"]` so an existing config file with no `providers` key keeps
    /// today's single-provider behavior unchanged — see the bun-distribution
    /// (now binary-distribution) spec's "Existing single-provider
    /// configuration keeps working unchanged" requirement.
    pub fn providers(&self) -> Vec<String> {
        self.comma_list("providers", &["claude"])
    }

    /// Claude config directories to poll, expanding `~`. Defaults to
    /// `~/.claude`, matching current single-plan behavior.
    pub fn claude_config_dirs(&self) -> Vec<PathBuf> {
        let raw = self.comma_list("config_dirs", &["~/.claude"]);
        raw.into_iter().map(expand_tilde).collect()
    }

    pub fn chime_enabled(&self) -> bool {
        self.get("chime").map(|v| v == "on").unwrap_or(false)
    }
}

fn expand_tilde(path: String) -> PathBuf {
    if let Some(rest) = path.strip_prefix("~/") {
        return dirs::home_dir().unwrap_or_default().join(rest);
    }
    if path == "~" {
        return dirs::home_dir().unwrap_or_default();
    }
    PathBuf::from(path)
}
