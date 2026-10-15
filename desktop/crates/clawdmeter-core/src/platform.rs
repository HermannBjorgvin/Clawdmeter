//! Small host queries: hour format and UTC offset.

use std::process::{Command, Stdio};

fn run(cmd: &str, args: &[&str]) -> Option<String> {
    let out = Command::new(cmd).args(args).stdin(Stdio::null()).stderr(Stdio::null()).output().ok()?;
    out.status.success().then(|| String::from_utf8_lossy(&out.stdout).into_owned())
}

/// True when a `t_fmt` / `sShortTime`-style format is 12-hour.
pub fn format_is_12h(fmt: &str) -> bool {
    fmt.contains("%p") || fmt.contains("%r") || fmt.contains("%I") || fmt.contains("tt")
}

/// Best-effort 12h/24h for `clock = auto`
/// (macOS defaults toggles, then `locale -k LC_TIME`; Windows `iTime`).
/// Defaults to 24h.
pub fn prefers_12h() -> bool {
    if cfg!(target_os = "macos") {
        if run("defaults", &["read", "-g", "AppleICUForce24HourTime"]).is_some_and(|s| s.trim() == "1") {
            return false;
        }
        if run("defaults", &["read", "-g", "AppleICUForce12HourTime"]).is_some_and(|s| s.trim() == "1") {
            return true;
        }
    }
    if cfg!(windows) {
        // iTime: "1" = 24-hour, "0" = 12-hour.
        return run("reg", &["query", r"HKCU\Control Panel\International", "/v", "iTime"])
            .is_some_and(|s| s.lines().any(|l| l.contains("iTime") && l.trim_end().ends_with('0')));
    }
    run("locale", &["-k", "LC_TIME"])
        .and_then(|s| s.lines().find(|l| l.starts_with("t_fmt=")).map(format_is_12h))
        .unwrap_or(false)
}

/// The local UTC offset in seconds right now.
pub fn utc_offset_secs() -> i64 {
    i64::from(chrono::Local::now().offset().local_minus_utc())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn formats() {
        assert!(format_is_12h("t_fmt=\"%r\""));
        assert!(format_is_12h("%I:%M:%S %p"));
        assert!(!format_is_12h("t_fmt=\"%T\""));
        assert!(!format_is_12h("%H:%M:%S"));
    }
}
