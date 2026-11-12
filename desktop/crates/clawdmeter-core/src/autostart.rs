//! Start at login: a systemd user unit or XDG autostart entry (Linux), a
//! LaunchAgent (macOS), an HKCU Run value (Windows).
//!
//! The text generators are pure and tested on every OS; `enable` / `disable`
//! / `is_enabled` act on the current OS only.

use std::fs;
use std::io;
use std::path::{Path, PathBuf};
use std::process::Command;

/// How the app is launched at login.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LaunchMode {
    /// The tray app (desktop sessions).
    Tray,
    /// Headless `--daemon` (servers, no tray).
    Daemon,
}

impl LaunchMode {
    fn args(self) -> &'static [&'static str] {
        match self {
            LaunchMode::Tray => &[],
            LaunchMode::Daemon => &["--daemon"],
        }
    }
}

/// systemd unit name.
pub const SYSTEMD_UNIT: &str = "clawdmeter.service";
/// The old bash daemon's unit; running both would fight over the device.
pub const LEGACY_SYSTEMD_UNIT: &str = "claude-usage-daemon.service";
/// LaunchAgent label.
pub const LAUNCH_AGENT_LABEL: &str = "com.clawdmeter.app";
/// The old Python daemon's LaunchAgent label.
pub const LEGACY_LAUNCH_AGENT_LABEL: &str = "com.user.claude-usage-daemon";
/// HKCU Run value name (same as the old Windows tray, which it replaces).
pub const RUN_VALUE: &str = "Clawdmeter";

/// Quote one argument for a systemd `ExecStart=` line.
fn systemd_quote(s: &str) -> String {
    let escaped = s.replace('\\', "\\\\").replace('"', "\\\"").replace('%', "%%").replace('$', "$$");
    format!("\"{escaped}\"")
}

/// The systemd user unit (`--daemon`, restarts on failure).
pub fn systemd_unit(exe: &Path) -> String {
    let mut exec = systemd_quote(&exe.to_string_lossy());
    for a in LaunchMode::Daemon.args() {
        exec.push(' ');
        exec.push_str(a);
    }
    format!(
        "[Unit]\n\
         Description=Clawdmeter daemon (Claude Code usage over BLE)\n\
         After=bluetooth.target\n\
         \n\
         [Service]\n\
         Type=simple\n\
         ExecStart={exec}\n\
         Restart=on-failure\n\
         RestartSec=5\n\
         \n\
         [Install]\n\
         WantedBy=default.target\n"
    )
}

/// XDG autostart entry for the tray app.
pub fn xdg_desktop_entry(exe: &Path) -> String {
    let path = exe.to_string_lossy();
    let quoted = if path.contains(' ') { format!("\"{path}\"") } else { path.into_owned() };
    format!(
        "[Desktop Entry]\n\
         Type=Application\n\
         Name=Clawdmeter\n\
         Comment=Claude Code usage on your desk\n\
         Exec={quoted}\n\
         Terminal=false\n\
         X-GNOME-Autostart-enabled=true\n"
    )
}

fn xml_escape(s: &str) -> String {
    s.replace('&', "&amp;").replace('<', "&lt;").replace('>', "&gt;").replace('"', "&quot;")
}

/// The LaunchAgent plist.
pub fn launch_agent_plist(exe: &Path, mode: LaunchMode, log_path: &Path) -> String {
    let mut args = format!("        <string>{}</string>\n", xml_escape(&exe.to_string_lossy()));
    for a in mode.args() {
        args.push_str(&format!("        <string>{a}</string>\n"));
    }
    let log = xml_escape(&log_path.to_string_lossy());
    format!(
        r#"<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>{LAUNCH_AGENT_LABEL}</string>
    <key>ProgramArguments</key>
    <array>
{args}    </array>
    <key>RunAtLoad</key>
    <true/>
    <key>KeepAlive</key>
    <dict>
        <key>SuccessfulExit</key>
        <false/>
    </dict>
    <key>ThrottleInterval</key>
    <integer>10</integer>
    <key>StandardOutPath</key>
    <string>{log}</string>
    <key>StandardErrorPath</key>
    <string>{log}</string>
</dict>
</plist>
"#
    )
}

/// The HKCU Run command line.
pub fn windows_run_command(exe: &Path, mode: LaunchMode) -> String {
    let mut cmd = format!("\"{}\"", exe.to_string_lossy());
    for a in mode.args() {
        cmd.push(' ');
        cmd.push_str(a);
    }
    cmd
}

/// The first argument of a `"quoted" rest` or `bare rest` command line,
/// with `unescape` applied inside quotes.
fn first_arg(line: &str, unescape: impl Fn(&str) -> String) -> Option<String> {
    let line = line.trim();
    if let Some(rest) = line.strip_prefix('"') {
        let mut end = None;
        let mut esc = false;
        for (i, c) in rest.char_indices() {
            match c {
                _ if esc => esc = false,
                '\\' => esc = true,
                '"' => {
                    end = Some(i);
                    break;
                }
                _ => {}
            }
        }
        return end.map(|e| unescape(&rest[..e]));
    }
    line.split_whitespace().next().map(str::to_owned)
}

/// The executable a [`systemd_unit`] starts.
pub fn exe_from_systemd_unit(text: &str) -> Option<PathBuf> {
    let line = text.lines().find_map(|l| l.strip_prefix("ExecStart="))?;
    let arg = first_arg(line, |s| {
        let mut out = String::new();
        let mut chars = s.chars();
        while let Some(c) = chars.next() {
            if c == '\\' {
                out.extend(chars.next());
            } else {
                out.push(c);
            }
        }
        out.replace("%%", "%").replace("$$", "$")
    })?;
    Some(PathBuf::from(arg))
}

/// The executable an [`xdg_desktop_entry`] runs.
pub fn exe_from_desktop_entry(text: &str) -> Option<PathBuf> {
    let line = text.lines().find_map(|l| l.strip_prefix("Exec="))?;
    first_arg(line, str::to_owned).map(PathBuf::from)
}

/// The executable a [`launch_agent_plist`] runs: the first
/// `ProgramArguments` string.
pub fn exe_from_plist(text: &str) -> Option<PathBuf> {
    let args = &text[text.find("<key>ProgramArguments</key>")?..];
    let start = args.find("<string>")? + "<string>".len();
    let end = start + args[start..].find("</string>")?;
    let s = args[start..end].replace("&quot;", "\"").replace("&gt;", ">").replace("&lt;", "<").replace("&amp;", "&");
    Some(PathBuf::from(s))
}

/// The executable a [`windows_run_command`] runs.
pub fn exe_from_run_command(cmd: &str) -> Option<PathBuf> {
    first_arg(cmd, str::to_owned).map(PathBuf::from)
}

/// Where the login entry lives for `mode` on this OS (`None` on Windows,
/// which uses the registry).
pub fn entry_path(home: &Path, mode: LaunchMode) -> Option<PathBuf> {
    if cfg!(target_os = "macos") {
        Some(home.join("Library/LaunchAgents").join(format!("{LAUNCH_AGENT_LABEL}.plist")))
    } else if cfg!(windows) {
        None
    } else {
        Some(match mode {
            LaunchMode::Daemon => home.join(".config/systemd/user").join(SYSTEMD_UNIT),
            LaunchMode::Tray => home.join(".config/autostart/clawdmeter.desktop"),
        })
    }
}

const RUN_KEY: &str = r"HKCU\Software\Microsoft\Windows\CurrentVersion\Run";

/// Keep a child from opening a console window: the app is a GUI-subsystem
/// process, so on Windows each `reg.exe` would otherwise get its own console.
pub(crate) fn no_console(cmd: &mut Command) -> &mut Command {
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        cmd.creation_flags(0x0800_0000); // CREATE_NO_WINDOW
    }
    cmd
}

fn run_quiet(cmd: &str, args: &[&str]) -> io::Result<bool> {
    let st = no_console(Command::new(cmd).args(args))
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null())
        .status()?;
    Ok(st.success())
}

fn write_file(path: &Path, text: &str) -> io::Result<()> {
    if let Some(d) = path.parent() {
        fs::create_dir_all(d)?;
    }
    fs::write(path, text)
}

/// Install and activate the login entry for `exe`.
pub fn enable(home: &Path, exe: &Path, mode: LaunchMode) -> io::Result<()> {
    if cfg!(windows) {
        let cmd = windows_run_command(exe, mode);
        if !run_quiet("reg", &["add", RUN_KEY, "/v", RUN_VALUE, "/t", "REG_SZ", "/d", &cmd, "/f"])? {
            return Err(io::Error::other("reg add failed"));
        }
        return Ok(());
    }
    let path = entry_path(home, mode).expect("unix has a path");
    if cfg!(target_os = "macos") {
        let log = home.join("Library/Logs/clawdmeter.log");
        write_file(&path, &launch_agent_plist(exe, mode, &log))?;
        // Best effort: load now so it runs without a re-login.
        let _ = run_quiet("launchctl", &["load", "-w", &path.to_string_lossy()]);
        return Ok(());
    }
    match mode {
        LaunchMode::Daemon => {
            write_file(&path, &systemd_unit(exe))?;
            let _ = run_quiet("systemctl", &["--user", "daemon-reload"]);
            if !run_quiet("systemctl", &["--user", "enable", SYSTEMD_UNIT])? {
                return Err(io::Error::other("systemctl --user enable failed"));
            }
        }
        LaunchMode::Tray => write_file(&path, &xdg_desktop_entry(exe))?,
    }
    Ok(())
}

/// Remove the login entry (idempotent).
pub fn disable(home: &Path, mode: LaunchMode) -> io::Result<()> {
    if cfg!(windows) {
        let _ = run_quiet("reg", &["delete", RUN_KEY, "/v", RUN_VALUE, "/f"]);
        return Ok(());
    }
    let path = entry_path(home, mode).expect("unix has a path");
    if cfg!(target_os = "macos") {
        let _ = run_quiet("launchctl", &["unload", "-w", &path.to_string_lossy()]);
    } else if mode == LaunchMode::Daemon {
        let _ = run_quiet("systemctl", &["--user", "disable", SYSTEMD_UNIT]);
    }
    match fs::remove_file(&path) {
        Err(e) if e.kind() != io::ErrorKind::NotFound => Err(e),
        _ => Ok(()),
    }
}

/// Whether the login entry is present. On Windows the Run value must be
/// ours: the old Python tray used the same value name.
pub fn is_enabled(home: &Path, mode: LaunchMode) -> bool {
    if cfg!(windows) {
        return windows_run_value().is_some_and(|cmd| !is_legacy_run_command(&cmd));
    }
    entry_path(home, mode).is_some_and(|p| p.exists())
}

/// The executable the installed login entry launches, if there is one.
pub fn entry_exe(home: &Path, mode: LaunchMode) -> Option<PathBuf> {
    if cfg!(windows) {
        return windows_run_value().filter(|c| !is_legacy_run_command(c)).and_then(|c| exe_from_run_command(&c));
    }
    let text = fs::read_to_string(entry_path(home, mode)?).ok()?;
    if cfg!(target_os = "macos") {
        exe_from_plist(&text)
    } else {
        match mode {
            LaunchMode::Daemon => exe_from_systemd_unit(&text),
            LaunchMode::Tray => exe_from_desktop_entry(&text),
        }
    }
}

/// Whether an installed login entry should be rewritten: it launches a
/// file that is gone, or something other than `want` (an AppImage mount, an
/// old Nix store path, a moved binary). `false` when there is no entry.
pub fn is_stale(home: &Path, mode: LaunchMode, want: &Path) -> bool {
    if !is_enabled(home, mode) {
        return false;
    }
    match entry_exe(home, mode) {
        // A copy on a disk image or in a temp folder won't be there later.
        Some(exe) => exe != want || !exe.exists() || crate::launch::unstable(&exe).is_some(),
        None => true,
    }
}

/// The current `Clawdmeter` Run value (Windows), if any.
fn windows_run_value() -> Option<String> {
    let out = no_console(Command::new("reg").args(["query", RUN_KEY, "/v", RUN_VALUE]))
        .stdin(std::process::Stdio::null())
        .stderr(std::process::Stdio::null())
        .output()
        .ok()?;
    out.status.success().then(|| parse_reg_value(&String::from_utf8_lossy(&out.stdout), RUN_VALUE)).flatten()
}

/// The data of value `name` in `reg query ... /v name` output:
/// `    Clawdmeter    REG_SZ    "C:\...\pythonw.exe" "C:\...\tray_windows.py"`.
pub fn parse_reg_value(out: &str, name: &str) -> Option<String> {
    out.lines().find_map(|l| {
        let rest = l.trim_start().strip_prefix(name)?;
        let rest = rest.trim_start();
        let (ty, data) = rest.split_once(char::is_whitespace)?;
        ty.starts_with("REG_").then(|| data.trim().to_owned())
    })
}

/// True for the old Windows tray's Run command (`pythonw.exe ...
/// daemon\tray_windows.py`), as opposed to the app's own.
pub fn is_legacy_run_command(cmd: &str) -> bool {
    let c = cmd.to_ascii_lowercase();
    c.contains("tray_windows.py") || c.contains("claude_usage_daemon_windows.py") || c.contains("pythonw")
}

/// An old daemon's login entry that is still installed.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum LegacyEntry {
    /// The bash daemon's systemd user unit (Linux).
    SystemdUnit(PathBuf),
    /// The Python daemon's LaunchAgent (macOS).
    LaunchAgent(PathBuf),
    /// The Python tray's `HKCU\...\Run\Clawdmeter` value (Windows); the
    /// command line it runs.
    RunValue(String),
}

impl LegacyEntry {
    /// Where it lives, for the UI.
    pub fn describe(&self) -> String {
        match self {
            LegacyEntry::SystemdUnit(p) | LegacyEntry::LaunchAgent(p) => p.display().to_string(),
            LegacyEntry::RunValue(_) => format!("{RUN_KEY}\\{RUN_VALUE}"),
        }
    }
}

/// The old daemon's login entry on this OS, if it is still installed.
pub fn legacy_entry(home: &Path) -> Option<LegacyEntry> {
    if cfg!(windows) {
        return windows_run_value().filter(|c| is_legacy_run_command(c)).map(LegacyEntry::RunValue);
    }
    let p = legacy_entry_path(home);
    if !p.exists() {
        return None;
    }
    Some(if cfg!(target_os = "macos") { LegacyEntry::LaunchAgent(p) } else { LegacyEntry::SystemdUnit(p) })
}

/// Where the old daemon's unit / LaunchAgent lives (Linux / macOS).
pub fn legacy_entry_path(home: &Path) -> PathBuf {
    if cfg!(target_os = "macos") {
        home.join("Library/LaunchAgents").join(format!("{LEGACY_LAUNCH_AGENT_LABEL}.plist"))
    } else {
        home.join(".config/systemd/user").join(LEGACY_SYSTEMD_UNIT)
    }
}

/// Stop the old daemon and remove its login entry (bash systemd unit,
/// Python LaunchAgent, or the Python tray's Run value) so it doesn't fight
/// the app for the device. Best effort; returns what it turned off.
///
/// The entry is removed, not just disabled, so [`legacy_entry`] stops
/// reporting it. A running Windows tray keeps running until it is quit or
/// the user logs out (it can't be told apart from other `pythonw.exe`s).
pub fn disable_legacy(home: &Path) -> Option<LegacyEntry> {
    disable_legacy_with(home, &mut |cmd, args| {
        let _ = run_quiet(cmd, args);
    })
}

/// [`disable_legacy`] with the service-manager commands handed to `run`
/// (tests record them instead of stopping a real service).
pub fn disable_legacy_with(home: &Path, run: &mut dyn FnMut(&str, &[&str])) -> Option<LegacyEntry> {
    let entry = legacy_entry(home)?;
    match &entry {
        LegacyEntry::SystemdUnit(p) => {
            run("systemctl", &["--user", "disable", "--now", LEGACY_SYSTEMD_UNIT]);
            let _ = fs::remove_file(p);
            run("systemctl", &["--user", "daemon-reload"]);
        }
        LegacyEntry::LaunchAgent(p) => {
            run("launchctl", &["unload", "-w", &p.to_string_lossy()]);
            let _ = fs::remove_file(p);
        }
        LegacyEntry::RunValue(_) => run("reg", &["delete", RUN_KEY, "/v", RUN_VALUE, "/f"]),
    }
    Some(entry)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn systemd_unit_text() {
        let u = systemd_unit(Path::new("/opt/My Apps/clawdmeter"));
        assert!(u.contains("ExecStart=\"/opt/My Apps/clawdmeter\" --daemon\n"));
        assert!(u.contains("Restart=on-failure"));
        assert!(u.contains("WantedBy=default.target"));
        assert!(systemd_unit(Path::new("/a%b")).contains("\"/a%%b\""));
    }

    #[test]
    fn desktop_entry_text() {
        assert!(xdg_desktop_entry(Path::new("/usr/bin/clawdmeter")).contains("Exec=/usr/bin/clawdmeter\n"));
        assert!(xdg_desktop_entry(Path::new("/a b/c")).contains("Exec=\"/a b/c\"\n"));
    }

    #[test]
    fn plist_text() {
        let p =
            launch_agent_plist(Path::new("/Applications/Clawdmeter.app/x&y"), LaunchMode::Daemon, Path::new("/l.log"));
        assert!(p.contains("<string>/Applications/Clawdmeter.app/x&amp;y</string>\n        <string>--daemon</string>"));
        assert!(p.contains(LAUNCH_AGENT_LABEL));
        let t = launch_agent_plist(Path::new("/x"), LaunchMode::Tray, Path::new("/l.log"));
        assert!(!t.contains("--daemon"));
    }

    #[test]
    fn windows_command() {
        assert_eq!(
            windows_run_command(Path::new(r"C:\Program Files\Clawdmeter\clawdmeter.exe"), LaunchMode::Tray),
            r#""C:\Program Files\Clawdmeter\clawdmeter.exe""#
        );
        assert!(windows_run_command(Path::new("c.exe"), LaunchMode::Daemon).ends_with(" --daemon"));
    }

    #[test]
    fn entries_round_trip_their_exe() {
        for exe in ["/usr/bin/clawdmeter", "/opt/My Apps/clawd\"meter", "/a%b/$c&<d>"] {
            let exe = Path::new(exe);
            assert_eq!(exe_from_systemd_unit(&systemd_unit(exe)).as_deref(), Some(exe));
            for mode in [LaunchMode::Tray, LaunchMode::Daemon] {
                assert_eq!(exe_from_plist(&launch_agent_plist(exe, mode, Path::new("/l.log"))).as_deref(), Some(exe));
            }
        }
        for exe in ["/usr/bin/clawdmeter", "/tmp/.mount_Clawd x/usr/bin/clawdmeter"] {
            let exe = Path::new(exe);
            assert_eq!(exe_from_desktop_entry(&xdg_desktop_entry(exe)).as_deref(), Some(exe));
        }
        let win = Path::new(r"C:\Program Files\Clawdmeter\clawdmeter.exe");
        for mode in [LaunchMode::Tray, LaunchMode::Daemon] {
            assert_eq!(exe_from_run_command(&windows_run_command(win, mode)).as_deref(), Some(win));
        }
        assert_eq!(exe_from_desktop_entry("[Desktop Entry]\nName=x\n"), None);
        assert_eq!(exe_from_run_command("\"unterminated"), None);
    }

    // Ported from test_windows_autostart.py: the Run value name and the
    // pythonw command the old tray wrote.
    #[test]
    fn windows_legacy_run_value() {
        let out = "\r\nHKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run\r\n    Clawdmeter    REG_SZ    \"C:\\Python313\\pythonw.exe\" \"C:\\Users\\me\\Clawdmeter\\daemon\\tray_windows.py\"\r\n\r\n";
        let cmd = parse_reg_value(out, RUN_VALUE).unwrap();
        assert_eq!(cmd, r#""C:\Python313\pythonw.exe" "C:\Users\me\Clawdmeter\daemon\tray_windows.py""#);
        assert!(is_legacy_run_command(&cmd));
        let ours = windows_run_command(Path::new(r"C:\Program Files\Clawdmeter\clawdmeter.exe"), LaunchMode::Tray);
        assert!(!is_legacy_run_command(&ours));
        assert_eq!(
            parse_reg_value(&format!("    Clawdmeter    REG_SZ    {ours}\r\n"), RUN_VALUE).as_deref(),
            Some(ours.as_str())
        );
        assert_eq!(
            parse_reg_value("ERROR: The system was unable to find the specified registry key or value.", RUN_VALUE),
            None
        );
        assert_eq!(parse_reg_value("    ClawdmeterX    REG_SZ    x", RUN_VALUE), None);
    }

    // Never runs a real systemctl: that would stop the developer's own
    // legacy daemon.
    #[cfg(target_os = "linux")]
    #[test]
    fn linux_legacy_unit_is_detected_and_removed() {
        let home = std::env::temp_dir().join(format!("cm-legacy-{}", std::process::id()));
        let _ = fs::remove_dir_all(&home);
        let mut ran: Vec<String> = Vec::new();
        let mut record = |c: &str, a: &[&str]| ran.push(format!("{c} {}", a.join(" ")));
        assert_eq!(legacy_entry(&home), None);
        assert_eq!(disable_legacy_with(&home, &mut record), None);
        let unit = home.join(".config/systemd/user").join(LEGACY_SYSTEMD_UNIT);
        write_file(&unit, "[Service]\nExecStart=/x/claude-usage-daemon.sh\n").unwrap();
        assert_eq!(legacy_entry(&home), Some(LegacyEntry::SystemdUnit(unit.clone())));
        assert_eq!(disable_legacy_with(&home, &mut record), Some(LegacyEntry::SystemdUnit(unit.clone())));
        assert!(!unit.exists());
        assert_eq!(legacy_entry(&home), None);
        assert_eq!(
            ran,
            [
                "systemctl --user disable --now claude-usage-daemon.service".to_owned(),
                "systemctl --user daemon-reload".to_owned()
            ]
        );
        let _ = fs::remove_dir_all(&home);
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn linux_paths_and_tray_enable_disable() {
        let home = std::env::temp_dir().join(format!("cm-auto-{}", std::process::id()));
        let _ = fs::remove_dir_all(&home);
        assert_eq!(
            entry_path(&home, LaunchMode::Daemon).unwrap(),
            home.join(".config/systemd/user/clawdmeter.service")
        );
        assert!(!is_enabled(&home, LaunchMode::Tray));
        enable(&home, Path::new("/usr/bin/clawdmeter"), LaunchMode::Tray).unwrap();
        assert!(is_enabled(&home, LaunchMode::Tray));
        assert_eq!(entry_exe(&home, LaunchMode::Tray).as_deref(), Some(Path::new("/usr/bin/clawdmeter")));
        let real = std::env::current_exe().unwrap();
        assert!(is_stale(&home, LaunchMode::Tray, &real), "points elsewhere");
        enable(&home, &real, LaunchMode::Tray).unwrap();
        assert!(!is_stale(&home, LaunchMode::Tray, &real));
        enable(&home, Path::new("/tmp/.mount_gone/usr/bin/clawdmeter"), LaunchMode::Tray).unwrap();
        assert!(is_stale(&home, LaunchMode::Tray, Path::new("/tmp/.mount_gone/usr/bin/clawdmeter")), "target missing");
        disable(&home, LaunchMode::Tray).unwrap();
        assert!(!is_enabled(&home, LaunchMode::Tray));
        disable(&home, LaunchMode::Tray).unwrap();
        let _ = fs::remove_dir_all(&home);
    }
}
