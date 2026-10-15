//! The `key = value` config file, shared with the settings window (and with
//! the bash / Python daemons it replaces, so an existing file keeps working).
//!
//! Reading matches those daemons: `#` starts a comment, whitespace and CR are
//! trimmed, keys compare case-insensitively, and the last occurrence of a key
//! wins. Writing edits
//! the file line by line so comments, blank lines, unknown keys and their
//! order survive a round trip through the GUI.

use std::fs;
use std::io;
use std::path::{Path, PathBuf};

use crate::paths::Paths;

/// Default seconds between `/usage` polls while a session is working
/// (`cadence::Cadence::default().working`).
pub const DEFAULT_POLL_INTERVAL: u64 = 180;
/// Default seconds between heartbeat replays.
pub const DEFAULT_HEARTBEAT_INTERVAL: u64 = 60;
/// Smallest accepted poll / heartbeat interval.
pub const MIN_INTERVAL: u64 = 10;
/// GitHub repo the flasher reads releases from.
pub const DEFAULT_RELEASE_REPO: &str = "HermannBjorgvin/Clawdmeter";
/// How lively Clawd is on the device, 1 (No-nonsense) to 5 (All-nonsense).
pub const LIVELINESS_MIN: u8 = 1;
/// See [`LIVELINESS_MIN`].
pub const LIVELINESS_MAX: u8 = 5;
/// Some-nonsense: the director's own timings.
pub const DEFAULT_LIVELINESS: u8 = 3;

/// A config file held as its original lines.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct ConfigFile {
    lines: Vec<String>,
    trailing_newline: bool,
}

/// Split one line into (key, value) when it is an active assignment.
fn split_assignment(line: &str) -> Option<(&str, &str)> {
    let line = line.trim_end_matches('\r');
    let code = match line.find('#') {
        Some(i) => &line[..i],
        None => line,
    };
    let (k, v) = code.split_once('=')?;
    let k = k.trim();
    if k.is_empty() || k.contains(char::is_whitespace) {
        return None;
    }
    Some((k, v.trim()))
}

impl ConfigFile {
    /// Parse file contents. Never fails: unparseable lines are kept verbatim.
    pub fn parse(text: &str) -> Self {
        let trailing_newline = text.ends_with('\n');
        let body = text.strip_suffix('\n').unwrap_or(text);
        let lines = if text.is_empty() { Vec::new() } else { body.split('\n').map(str::to_owned).collect() };
        ConfigFile { lines, trailing_newline: trailing_newline || text.is_empty() }
    }

    /// Load from disk; a missing file is an empty config.
    pub fn load(path: &Path) -> io::Result<Self> {
        match fs::read_to_string(path) {
            Ok(t) => Ok(Self::parse(&t)),
            Err(e) if e.kind() == io::ErrorKind::NotFound => Ok(Self::default()),
            Err(e) => Err(e),
        }
    }

    /// Write atomically (temp file + rename), creating the parent dir.
    pub fn save(&self, path: &Path) -> io::Result<()> {
        if let Some(dir) = path.parent() {
            fs::create_dir_all(dir)?;
        }
        let tmp: PathBuf = path.with_extension("tmp");
        fs::write(&tmp, self.render())?;
        fs::rename(&tmp, path)
    }

    /// The file contents.
    pub fn render(&self) -> String {
        let mut s = self.lines.join("\n");
        if self.trailing_newline && !self.lines.is_empty() {
            s.push('\n');
        }
        s
    }

    /// The value of `key` (last occurrence wins), comment and whitespace stripped.
    pub fn get(&self, key: &str) -> Option<&str> {
        self.lines
            .iter()
            .rev()
            .filter_map(|l| split_assignment(l))
            .find(|(k, _)| k.eq_ignore_ascii_case(key))
            .map(|(_, v)| v)
    }

    /// Set `key`. Rewrites the last active line for it in place (keeping its
    /// indentation and trailing comment); otherwise inserts after a commented
    /// example (`# key = ...`) if there is one, or appends.
    pub fn set(&mut self, key: &str, value: &str) {
        let active =
            self.lines.iter().rposition(|l| split_assignment(l).is_some_and(|(k, _)| k.eq_ignore_ascii_case(key)));
        if let Some(i) = active {
            let line = self.lines[i].trim_end_matches('\r').to_owned();
            let cr = if self.lines[i].ends_with('\r') { "\r" } else { "" };
            let indent: String = line.chars().take_while(|c| c.is_whitespace()).collect();
            let comment = line.find('#').map(|j| line[j..].to_owned());
            let mut new = format!("{indent}{key} = {value}");
            if let Some(c) = comment {
                new.push(' ');
                new.push_str(&c);
            }
            new.push_str(cr);
            self.lines[i] = new;
            return;
        }
        let example = self.lines.iter().rposition(|l| {
            let t = l.trim_start();
            t.strip_prefix('#')
                .and_then(|rest| split_assignment(rest.trim_start()))
                .is_some_and(|(k, _)| k.eq_ignore_ascii_case(key))
        });
        let new = format!("{key} = {value}");
        match example {
            Some(i) => self.lines.insert(i + 1, new),
            None => self.lines.push(new),
        }
        self.trailing_newline = true;
    }

    /// Remove every active line for `key`. Comments are left alone.
    pub fn remove(&mut self, key: &str) {
        self.lines.retain(|l| !split_assignment(l).is_some_and(|(k, _)| k.eq_ignore_ascii_case(key)));
    }

    /// All active `(key, value)` pairs in file order (duplicates included).
    pub fn entries(&self) -> impl Iterator<Item = (&str, &str)> {
        self.lines.iter().filter_map(|l| split_assignment(l))
    }
}

/// `clock` option: whether and how the device shows a wall clock.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum ClockMode {
    /// No clock; the device shows "Usage" (default).
    #[default]
    Off,
    /// Show a clock, 12/24h from the locale.
    Auto,
    /// Forced 12-hour.
    H12,
    /// Forced 24-hour.
    H24,
}

impl ClockMode {
    /// Parse a config value; anything unknown is `Off`, like the daemons.
    pub fn parse(v: &str) -> Self {
        match v.trim().to_ascii_lowercase().as_str() {
            "auto" => ClockMode::Auto,
            "12" => ClockMode::H12,
            "24" => ClockMode::H24,
            _ => ClockMode::Off,
        }
    }

    /// The config-file spelling.
    pub fn as_str(self) -> &'static str {
        match self {
            ClockMode::Off => "off",
            ClockMode::Auto => "auto",
            ClockMode::H12 => "12",
            ClockMode::H24 => "24",
        }
    }
}

/// `attention_anim`: how long Clawd points and waves on the device when
/// Claude needs you. Only his animation: the alert itself is unchanged.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum AttentionAnim {
    /// The whole alert, 2 minutes (default).
    #[default]
    Min2,
    /// 30 seconds.
    S30,
    /// 10 seconds.
    S10,
    /// No animation.
    Off,
}

impl AttentionAnim {
    /// Every option, in the order the Device page shows them.
    pub const ALL: [AttentionAnim; 4] =
        [AttentionAnim::Off, AttentionAnim::S10, AttentionAnim::S30, AttentionAnim::Min2];

    /// Parse a config value; anything unknown is the default.
    pub fn parse(v: &str) -> Self {
        match v.trim().to_ascii_lowercase().as_str() {
            "30s" => AttentionAnim::S30,
            "10s" => AttentionAnim::S10,
            "off" => AttentionAnim::Off,
            _ => AttentionAnim::Min2,
        }
    }

    /// The config-file spelling, also the Device page's label.
    pub fn as_str(self) -> &'static str {
        match self {
            AttentionAnim::Min2 => "2m",
            AttentionAnim::S30 => "30s",
            AttentionAnim::S10 => "10s",
            AttentionAnim::Off => "off",
        }
    }

    /// The payload's `aa`: seconds, 0 = off.
    pub fn seconds(self) -> u16 {
        match self {
            AttentionAnim::Min2 => 120,
            AttentionAnim::S30 => 30,
            AttentionAnim::S10 => 10,
            AttentionAnim::Off => 0,
        }
    }
}

/// Typed view of the config, with the old daemons' defaults and clamps.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Settings {
    /// Claude config dirs to poll (accounts). Default `[~/.claude]`.
    pub config_dirs: Vec<PathBuf>,
    /// `/usage` poll interval while a session is working (s, >= 10).
    pub poll_interval: u64,
    /// True when `poll_interval` came from the file or the env var, in which
    /// case it also replaces the 2-minute no-hooks cadence.
    pub poll_interval_explicit: bool,
    /// Heartbeat replay interval (s, >= 10).
    pub heartbeat_interval: u64,
    /// The device chimes when the session window resets (`c`;
    /// `reset_chime`, formerly `chime`).
    pub reset_chime: bool,
    /// The device chimes for an attention alert (`b`; `alert_chime`,
    /// formerly `beep`).
    pub alert_chime: bool,
    /// This computer chimes for an attention alert (`desktop_chime`).
    pub desktop_chime: bool,
    /// Wall clock on the device (`t`/`tf`).
    pub clock: ClockMode,
    /// `owner/repo` for firmware releases.
    pub release_repo: String,
    /// Explicit path to the `claude` CLI, if the user set one.
    pub claude_path: Option<PathBuf>,
    /// Daily firmware update check (default on).
    pub update_check: bool,
    /// Which kinds of "Claude needs you" alert reach the device and tray.
    pub alert_on: AlertOn,
    /// How lively Clawd is on the device (`lv`, 1-5, default 3).
    pub liveliness: u8,
    /// How long Clawd points and waves for an alert (`aa`, default 2m).
    pub attention_anim: AttentionAnim,
}

/// `alert_on`: the alert kinds that show, as a comma list of `permission`,
/// `question`, `idle`, `elicitation` (`none` for no alerts). Default: all
/// but `idle`. Other notification types follow `permission`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct AlertOn {
    /// Permission requests (and unrecognised notification types).
    pub permission: bool,
    /// AskUserQuestion.
    pub question: bool,
    /// The prompt waiting for input.
    pub idle: bool,
    /// MCP input requests.
    pub elicitation: bool,
}

impl Default for AlertOn {
    fn default() -> Self {
        AlertOn { permission: true, question: true, idle: false, elicitation: true }
    }
}

impl AlertOn {
    /// Parse the comma list; unknown words are ignored.
    pub fn parse(v: &str) -> Self {
        let mut a = AlertOn { permission: false, question: false, idle: false, elicitation: false };
        for w in v.split(',').map(|w| w.trim().to_ascii_lowercase()) {
            match w.as_str() {
                "permission" => a.permission = true,
                "question" => a.question = true,
                "idle" => a.idle = true,
                "elicitation" => a.elicitation = true,
                _ => {}
            }
        }
        a
    }

    /// The comma list ([`AlertOn::parse`] reads it back).
    pub fn as_config(&self) -> String {
        let on: Vec<&str> = [
            (self.permission, "permission"),
            (self.question, "question"),
            (self.idle, "idle"),
            (self.elicitation, "elicitation"),
        ]
        .into_iter()
        .filter_map(|(on, w)| on.then_some(w))
        .collect();
        if on.is_empty() {
            "none".into()
        } else {
            on.join(", ")
        }
    }

    /// Whether an alert of `kind` shows.
    pub fn allows(&self, kind: crate::attention::AlertKind) -> bool {
        use crate::attention::AlertKind as K;
        match kind {
            K::Permission | K::Other => self.permission,
            K::Question => self.question,
            K::Idle => self.idle,
            K::Elicitation => self.elicitation,
        }
    }
}

/// Parse `liveliness`: a whole number, clamped to 1-5; anything else is the
/// default.
pub fn parse_liveliness(v: &str) -> u8 {
    match v.trim().parse::<i64>() {
        Ok(n) => n.clamp(LIVELINESS_MIN as i64, LIVELINESS_MAX as i64) as u8,
        Err(_) => DEFAULT_LIVELINESS,
    }
}

/// Parse an interval like the bash daemon: digits only, clamped to >= 10.
pub fn parse_interval(v: &str) -> Option<u64> {
    let v = v.trim();
    if v.is_empty() || !v.bytes().all(|b| b.is_ascii_digit()) {
        return None;
    }
    // All digits but too long for u64 is still "a big number".
    Some(v.parse::<u64>().unwrap_or(u64::MAX).max(MIN_INTERVAL))
}

fn on_off(v: Option<&str>, default: bool) -> bool {
    match v.map(|s| s.trim().to_ascii_lowercase()) {
        Some(s) if s == "on" => true,
        Some(s) if s == "off" => false,
        Some(_) => false,
        None => default,
    }
}

impl Settings {
    /// Build from a parsed file. `env_poll` is the `POLL_INTERVAL` env var,
    /// which seeds the default the file overrides (bash daemon behaviour).
    pub fn from_config(cfg: &ConfigFile, paths: &Paths, env_poll: Option<&str>) -> Self {
        let env_default = env_poll.and_then(parse_interval);
        let file_poll = cfg.get("poll_interval").and_then(parse_interval);
        let poll_interval = file_poll.or(env_default).unwrap_or(DEFAULT_POLL_INTERVAL);
        let heartbeat_interval =
            cfg.get("heartbeat_interval").and_then(parse_interval).unwrap_or(DEFAULT_HEARTBEAT_INTERVAL);
        Settings {
            config_dirs: parse_config_dirs(cfg.get("config_dirs"), paths),
            poll_interval,
            poll_interval_explicit: file_poll.is_some() || env_default.is_some(),
            heartbeat_interval,
            // Only an explicit "on" enables (bash read_onoff_setting). The
            // old `chime` / `beep` keys still count when the new ones are absent.
            reset_chime: on_off(cfg.get("reset_chime").or_else(|| cfg.get("chime")), false),
            alert_chime: on_off(cfg.get("alert_chime").or_else(|| cfg.get("beep")), false),
            desktop_chime: on_off(cfg.get("desktop_chime"), false),
            clock: cfg.get("clock").map(ClockMode::parse).unwrap_or_default(),
            release_repo: cfg
                .get("release_repo")
                .filter(|s| s.split('/').count() == 2 && !s.starts_with('/') && !s.ends_with('/'))
                .unwrap_or(DEFAULT_RELEASE_REPO)
                .to_owned(),
            claude_path: cfg.get("claude_path").filter(|s| !s.is_empty()).map(|s| paths.expand_tilde(s)),
            update_check: on_off(cfg.get("update_check"), true),
            alert_on: cfg.get("alert_on").map(AlertOn::parse).unwrap_or_default(),
            liveliness: cfg.get("liveliness").map(parse_liveliness).unwrap_or(DEFAULT_LIVELINESS),
            attention_anim: cfg.get("attention_anim").map(AttentionAnim::parse).unwrap_or_default(),
        }
    }

    /// Load the user's config file (missing file → defaults), with the
    /// environment's `POLL_INTERVAL` and `CLAUDE_CONFIG_DIR` defaults.
    pub fn load(paths: &Paths) -> io::Result<Self> {
        let cfg = ConfigFile::load(&paths.config_file)?;
        let env = std::env::var("POLL_INTERVAL").ok();
        let mut s = Self::from_config(&cfg, paths, env.as_deref());
        s.apply_env_config_dir(&cfg, paths, std::env::var("CLAUDE_CONFIG_DIR").ok().as_deref());
        Ok(s)
    }

    /// With no `config_dirs` in the file, a `CLAUDE_CONFIG_DIR` in the
    /// environment names the account to poll: it is the dir the user's own
    /// `claude` uses (the Windows daemon read its token from there too).
    /// Only the daemon applies this; the settings window shows and saves the
    /// file's value, so saving never pins the env dir into the file.
    pub fn apply_env_config_dir(&mut self, cfg: &ConfigFile, paths: &Paths, env_dir: Option<&str>) {
        if cfg.get("config_dirs").is_some_and(|v| !v.trim().is_empty()) {
            return;
        }
        if let Some(d) = env_dir.map(str::trim).filter(|d| !d.is_empty()) {
            self.config_dirs = vec![paths.expand_tilde(d)];
        }
    }

    /// Write these settings into `cfg`, touching only keys whose value differs
    /// from what the file already says (so a hand-written `reset_chime = off` stays
    /// as written and absent keys at their defaults stay absent).
    pub fn apply_to(&self, cfg: &mut ConfigFile, paths: &Paths) {
        let current = Settings::from_config(cfg, paths, None);
        if current.config_dirs != self.config_dirs {
            let home = &paths.home;
            let list: Vec<String> = self
                .config_dirs
                .iter()
                .map(|d| match d.strip_prefix(home) {
                    Ok(rest) if rest.as_os_str().is_empty() => "~".to_owned(),
                    Ok(rest) => format!("~/{}", rest.to_string_lossy().replace('\\', "/")),
                    Err(_) => d.to_string_lossy().into_owned(),
                })
                .collect();
            cfg.set("config_dirs", &list.join(", "));
        }
        if current.poll_interval != self.poll_interval {
            cfg.set("poll_interval", &self.poll_interval.to_string());
        }
        if current.heartbeat_interval != self.heartbeat_interval {
            cfg.set("heartbeat_interval", &self.heartbeat_interval.to_string());
        }
        let oo = |b: bool| if b { "on" } else { "off" };
        // A changed chime is written under its new name; the old one goes.
        for (key, old, now, was) in [
            ("reset_chime", "chime", self.reset_chime, current.reset_chime),
            ("alert_chime", "beep", self.alert_chime, current.alert_chime),
            ("desktop_chime", "", self.desktop_chime, current.desktop_chime),
        ] {
            if now != was {
                cfg.set(key, oo(now));
                if !old.is_empty() {
                    cfg.remove(old);
                }
            }
        }
        if current.clock != self.clock {
            cfg.set("clock", self.clock.as_str());
        }
        if current.release_repo != self.release_repo {
            cfg.set("release_repo", &self.release_repo);
        }
        if current.claude_path != self.claude_path {
            match &self.claude_path {
                Some(p) => cfg.set("claude_path", &p.to_string_lossy()),
                None => cfg.remove("claude_path"),
            }
        }
        if current.update_check != self.update_check {
            cfg.set("update_check", oo(self.update_check));
        }
        if current.alert_on != self.alert_on {
            cfg.set("alert_on", &self.alert_on.as_config());
        }
        if current.liveliness != self.liveliness {
            cfg.set("liveliness", &self.liveliness.to_string());
        }
        if current.attention_anim != self.attention_anim {
            cfg.set("attention_anim", self.attention_anim.as_str());
        }
    }
}

/// `config_dirs`: comma list, `~` expanded, blanks dropped; default `[~/.claude]`.
pub fn parse_config_dirs(raw: Option<&str>, paths: &Paths) -> Vec<PathBuf> {
    let dirs: Vec<PathBuf> =
        raw.unwrap_or("").split(',').map(str::trim).filter(|s| !s.is_empty()).map(|s| paths.expand_tilde(s)).collect();
    if dirs.is_empty() {
        vec![paths.home.join(".claude")]
    } else {
        dirs
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn paths() -> Paths {
        Paths::under(Path::new("/home/u"))
    }

    fn settings(text: &str) -> Settings {
        Settings::from_config(&ConfigFile::parse(text), &paths(), None)
    }

    #[test]
    fn alert_kinds_round_trip() {
        use crate::attention::AlertKind;
        let d = settings("").alert_on;
        assert_eq!(d, AlertOn::default());
        assert!(d.allows(AlertKind::Question) && !d.allows(AlertKind::Idle));
        assert!(d.allows(AlertKind::Other), "unknown types follow permission");
        let s = settings("alert_on = idle, Question\n").alert_on;
        assert_eq!(s, AlertOn { permission: false, question: true, idle: true, elicitation: false });
        assert!(!s.allows(AlertKind::Other));
        assert_eq!(AlertOn::parse(&s.as_config()), s);
        let none = settings("alert_on = none\n").alert_on;
        assert_eq!(none.as_config(), "none");
        assert_eq!(AlertOn::parse(&none.as_config()), none);

        // Saving writes the key only when it differs from the file.
        let mut cfg = ConfigFile::parse("# keep\n");
        let mut s = Settings::from_config(&cfg, &paths(), None);
        s.apply_to(&mut cfg, &paths());
        assert!(cfg.get("alert_on").is_none(), "defaults stay out of the file");
        s.alert_on.idle = true;
        s.apply_to(&mut cfg, &paths());
        assert_eq!(cfg.get("alert_on"), Some("permission, question, idle, elicitation"));
    }

    // Ported from the legacy daemon test test_bash_heartbeat.sh (interval parsing).
    #[test]
    fn intervals_like_bash() {
        let s = settings("");
        assert_eq!((s.poll_interval, s.heartbeat_interval), (180, 60));
        assert!(!s.poll_interval_explicit);
        let s = settings("poll_interval = 300\nheartbeat_interval = 45\n");
        assert_eq!((s.poll_interval, s.heartbeat_interval), (300, 45));
        assert!(s.poll_interval_explicit);
        assert_eq!(settings("  poll_interval=120 # comment\r\n").poll_interval, 120);
        let s = settings("poll_interval = 3\nheartbeat_interval = 0\n");
        assert_eq!((s.poll_interval, s.heartbeat_interval), (10, 10));
        assert_eq!(settings("poll_interval = abc\n").poll_interval, 180);
        assert_eq!(settings("poll_interval = 30\npoll_interval = 90\n").poll_interval, 90);
    }

    #[test]
    fn env_poll_interval_seeds_default() {
        let cfg = ConfigFile::parse("");
        let p = paths();
        assert_eq!(Settings::from_config(&cfg, &p, Some("45")).poll_interval, 45);
        assert_eq!(Settings::from_config(&cfg, &p, Some("1")).poll_interval, 10);
        assert_eq!(Settings::from_config(&cfg, &p, Some("abc")).poll_interval, 180);
        assert_eq!(Settings::from_config(&cfg, &p, Some("-5")).poll_interval, 180);
        assert_eq!(Settings::from_config(&cfg, &p, Some("")).poll_interval, 180);
        let file = ConfigFile::parse("poll_interval = 200\n");
        assert_eq!(Settings::from_config(&file, &p, Some("45")).poll_interval, 200);
    }

    // Ported from the legacy daemon test test_macos_multidir.py (read_config_dirs).
    #[test]
    fn config_dirs() {
        assert_eq!(settings("").config_dirs, vec![PathBuf::from("/home/u/.claude")]);
        assert_eq!(settings("clock = auto\nchime = on\n").config_dirs, vec![PathBuf::from("/home/u/.claude")]);
        assert_eq!(
            settings("config_dirs = ~/.claude, ~/.claude-work  # two plans\n").config_dirs,
            vec![PathBuf::from("/home/u/.claude"), PathBuf::from("/home/u/.claude-work")]
        );
        assert_eq!(settings("config_dirs = , ,\n").config_dirs, vec![PathBuf::from("/home/u/.claude")]);
    }

    // The Windows daemon read the token from $CLAUDE_CONFIG_DIR when set
    // (test_windows_token.py::test_read_token_config_dir_override).
    #[test]
    fn env_config_dir_is_the_default_account() {
        let p = paths();
        let with_env = |text: &str, env: Option<&str>| {
            let cfg = ConfigFile::parse(text);
            let mut s = Settings::from_config(&cfg, &p, None);
            s.apply_env_config_dir(&cfg, &p, env);
            s.config_dirs
        };
        assert_eq!(with_env("", Some("~/.claude-work")), vec![PathBuf::from("/home/u/.claude-work")]);
        assert_eq!(with_env("", Some("/abs/dir")), vec![PathBuf::from("/abs/dir")]);
        assert_eq!(with_env("", Some("  ")), vec![PathBuf::from("/home/u/.claude")]);
        assert_eq!(with_env("", None), vec![PathBuf::from("/home/u/.claude")]);
        // The file wins.
        assert_eq!(with_env("config_dirs = ~/.claude\n", Some("/abs/dir")), vec![PathBuf::from("/home/u/.claude")]);
        assert_eq!(with_env("config_dirs =\n", Some("/abs/dir")), vec![PathBuf::from("/abs/dir")]);
    }

    #[test]
    fn on_off_and_clock() {
        let s = settings("chime = ON\nbeep = yes\nclock = 12\n");
        assert!(s.reset_chime, "the old key still counts");
        assert!(!s.alert_chime);
        assert!(!s.desktop_chime, "off by default");
        assert_eq!(s.clock, ClockMode::H12);
        let s = settings("chime = on\nreset_chime = off\nbeep = on\ndesktop_chime = on\n");
        assert!(!s.reset_chime, "the new key wins");
        assert!(s.alert_chime && s.desktop_chime);
        assert_eq!(settings("clock = weird").clock, ClockMode::Off);
        assert!(settings("").update_check);
        assert_eq!(settings("").release_repo, DEFAULT_RELEASE_REPO);
        assert_eq!(settings("release_repo = me/fork").release_repo, "me/fork");
        assert_eq!(settings("release_repo = nonsense").release_repo, DEFAULT_RELEASE_REPO);
    }

    #[test]
    fn liveliness() {
        assert_eq!(settings("").liveliness, DEFAULT_LIVELINESS);
        assert_eq!(settings("liveliness = 5\n").liveliness, 5);
        assert_eq!(settings("liveliness = 0\n").liveliness, 1, "clamped");
        assert_eq!(settings("liveliness = 9\n").liveliness, 5, "clamped");
        assert_eq!(settings("liveliness = lots\n").liveliness, DEFAULT_LIVELINESS);

        let mut cfg = ConfigFile::parse("");
        let mut s = Settings::from_config(&cfg, &paths(), None);
        s.apply_to(&mut cfg, &paths());
        assert!(cfg.get("liveliness").is_none(), "the default stays out of the file");
        s.liveliness = 1;
        s.apply_to(&mut cfg, &paths());
        assert_eq!(cfg.get("liveliness"), Some("1"));
        assert_eq!(Settings::from_config(&cfg, &paths(), None).liveliness, 1);
    }

    #[test]
    fn attention_anim() {
        assert_eq!(settings("").attention_anim, AttentionAnim::Min2);
        assert_eq!(settings("attention_anim = 30s\n").attention_anim, AttentionAnim::S30);
        assert_eq!(settings("attention_anim = 10S\n").attention_anim, AttentionAnim::S10);
        assert_eq!(settings("attention_anim = off\n").attention_anim, AttentionAnim::Off);
        assert_eq!(settings("attention_anim = forever\n").attention_anim, AttentionAnim::Min2);
        let secs: Vec<u16> = AttentionAnim::ALL.iter().map(|a| a.seconds()).collect();
        assert_eq!(secs, [0, 10, 30, 120]);
        for a in AttentionAnim::ALL {
            assert_eq!(AttentionAnim::parse(a.as_str()), a, "round trip");
        }

        let mut cfg = ConfigFile::parse("");
        let mut s = Settings::from_config(&cfg, &paths(), None);
        s.apply_to(&mut cfg, &paths());
        assert!(cfg.get("attention_anim").is_none(), "the default stays out of the file");
        s.attention_anim = AttentionAnim::Off;
        s.apply_to(&mut cfg, &paths());
        assert_eq!(cfg.get("attention_anim"), Some("off"));
        assert_eq!(Settings::from_config(&cfg, &paths(), None).attention_anim, AttentionAnim::Off);
    }

    #[test]
    fn keys_are_case_insensitive_and_comments_ignored() {
        let c = ConfigFile::parse("# chime = on\nCHIME = on\n");
        assert_eq!(c.get("chime"), Some("on"));
        let c = ConfigFile::parse("# chime = on\n");
        assert_eq!(c.get("chime"), None);
    }

    #[test]
    fn set_preserves_everything_else() {
        let text = "# header\n\nchime = off # quiet\nfoo = bar\n# beep = on\nclock = off\n";
        let mut c = ConfigFile::parse(text);
        c.set("chime", "on");
        c.set("beep", "on");
        c.set("poll_interval", "120");
        assert_eq!(
            c.render(),
            "# header\n\nchime = on # quiet\nfoo = bar\n# beep = on\nbeep = on\nclock = off\npoll_interval = 120\n"
        );
        c.remove("foo");
        assert!(c.get("foo").is_none());
        assert!(c.render().contains("# header"));
    }

    #[test]
    fn set_crlf_and_empty() {
        let mut c = ConfigFile::parse("chime = off\r\n");
        c.set("chime", "on");
        assert_eq!(c.render(), "chime = on\r\n");
        let mut e = ConfigFile::parse("");
        e.set("beep", "on");
        assert_eq!(e.render(), "beep = on\n");
        let mut n = ConfigFile::parse("a = 1");
        n.set("b", "2");
        assert_eq!(n.render(), "a = 1\nb = 2\n");
    }

    #[test]
    fn apply_only_touches_changed_keys() {
        let p = paths();
        let mut c = ConfigFile::parse("# mine\nchime = off\nunknown = 7\n");
        let mut s = Settings::from_config(&c, &p, None);
        s.alert_chime = true;
        s.config_dirs = vec![PathBuf::from("/home/u/.claude"), PathBuf::from("/home/u/.claude-work")];
        s.apply_to(&mut c, &p);
        let out = c.render();
        assert!(out.starts_with("# mine\nchime = off\nunknown = 7\n"), "{out}");
        assert!(out.contains("alert_chime = on\n") && !out.contains("beep"), "{out}");
        assert!(out.contains("config_dirs = ~/.claude, ~/.claude-work\n"));
        assert!(!out.contains("poll_interval"));
        let back = Settings::from_config(&c, &p, None);
        assert_eq!(back, s);
    }

    #[test]
    fn save_and_load_round_trip() {
        let dir = std::env::temp_dir().join(format!("cm-cfg-{}", std::process::id()));
        let path = dir.join("sub/config");
        let mut c = ConfigFile::parse("# x\nchime = on\n");
        c.set("beep", "on");
        c.save(&path).unwrap();
        assert_eq!(ConfigFile::load(&path).unwrap(), c);
        assert_eq!(ConfigFile::load(&dir.join("missing")).unwrap(), ConfigFile::default());
        let _ = fs::remove_dir_all(&dir);
    }
}
