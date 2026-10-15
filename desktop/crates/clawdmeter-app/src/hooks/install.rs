//! Wire `clawdmeter hook …` into each account's `settings.json`.
//!
//! Claude Code reads `settings.json` per config dir, so this runs per
//! account. It merges into existing hooks, produces before/after text for the
//! diff view, keeps a `.bak`, and only touches entries it recognises:
//!
//! * **ours**: `<path to a clawdmeter binary> hook <mode>`;
//! * **legacy**: anything running `attention-hook.sh` (the old README's
//!   manual setup and the previous installer). These are migrated in place.
//!
//! Everything else is left exactly as it is.

use std::fs;
use std::io;
use std::path::{Path, PathBuf};

use serde_json::{json, Map, Value};

/// The legacy shell hook's file name.
pub const LEGACY_SCRIPT: &str = "attention-hook.sh";

/// One hook the app installs.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct HookSpec {
    /// Hook event name.
    pub event: &'static str,
    /// Tool matcher (PreToolUse only).
    pub matcher: Option<&'static str>,
    /// Arguments after `hook`.
    pub args: &'static str,
}

/// The five hooks: the attention card's four plus Stop, all logging events.
pub const HOOKS: [HookSpec; 5] = [
    HookSpec { event: "UserPromptSubmit", matcher: None, args: "clear event" },
    HookSpec { event: "PostToolUse", matcher: None, args: "clear event" },
    HookSpec { event: "Stop", matcher: None, args: "event" },
    HookSpec { event: "Notification", matcher: None, args: "alert event" },
    HookSpec { event: "PreToolUse", matcher: Some("AskUserQuestion"), args: "alert event" },
];

/// The binary hooks should run: the path that outlives this process (an
/// AppImage's copy under `state_dir`, a Nix profile's `bin/clawdmeter`,
/// else this executable). See [`clawdmeter_core::launch::hook_binary`].
pub fn hook_binary(state_dir: &Path) -> io::Result<PathBuf> {
    clawdmeter_core::launch::hook_binary(state_dir, crate::version())
}

/// The command line for one hook. Windows paths are double-quoted with
/// forward slashes, which both Git Bash and cmd accept; POSIX paths are
/// single-quoted when they hold anything a shell would interpret.
pub fn hook_command(exe: &Path, args: &str) -> String {
    format!("{} hook {args}", quote_exe(exe))
}

fn quote_exe(exe: &Path) -> String {
    let p = exe.to_string_lossy();
    if cfg!(windows) {
        return format!("\"{}\"", p.replace('\\', "/"));
    }
    let safe = |c: char| c.is_ascii_alphanumeric() || "/._-+,:@%".contains(c);
    if !p.is_empty() && p.chars().all(safe) {
        p.into_owned()
    } else {
        format!("'{}'", p.replace('\'', r"'\''"))
    }
}

/// Split the first shell word off a command (`"a b" rest`, `'a b' rest`,
/// `a rest`). Returns (word, rest).
fn first_word(cmd: &str) -> Option<(String, &str)> {
    let cmd = cmd.trim_start();
    let mut chars = cmd.char_indices();
    let (_, c0) = chars.next()?;
    if c0 == '"' || c0 == '\'' {
        // Good enough for paths: handles POSIX '\'' splices.
        let mut word = String::new();
        let mut quote = Some(c0);
        let mut end = cmd.len();
        let mut it = cmd[1..].char_indices().peekable();
        while let Some((i, c)) = it.next() {
            match (quote, c) {
                (Some(q), c) if c == q => quote = None,
                (Some(_), c) => word.push(c),
                (None, '\\') => {
                    if let Some((_, n)) = it.next() {
                        word.push(n);
                    }
                }
                (None, '"') | (None, '\'') => quote = Some(c),
                (None, c) if c.is_whitespace() => {
                    end = i + 1;
                    break;
                }
                (None, c) => word.push(c),
            }
        }
        Some((word, &cmd[end..]))
    } else {
        let end = cmd.find(char::is_whitespace).unwrap_or(cmd.len());
        Some((cmd[..end].to_owned(), &cmd[end..]))
    }
}

/// What a hook command is, as far as we're concerned.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Owner {
    /// `<exe> hook <args>`.
    Ours {
        /// The binary path as written (unquoted).
        exe: String,
        /// The words after `hook`.
        args: String,
    },
    /// Runs `attention-hook.sh`.
    Legacy,
    /// Someone else's hook.
    Foreign,
}

fn basename(word: &str) -> String {
    word.rsplit(['/', '\\']).next().unwrap_or("").to_ascii_lowercase()
}

/// `[sh|bash] <dir>/attention-hook.sh <alert|clear|event>…`, and nothing
/// else: a user's own command that merely mentions the script (a wrapper, a
/// `&&` chain) stays foreign and is never rewritten.
fn is_legacy(cmd: &str) -> bool {
    let Some((mut word, mut rest)) = first_word(cmd) else { return false };
    if matches!(basename(&word).as_str(), "sh" | "bash" | "dash" | "zsh") {
        let Some((w, r)) = first_word(rest) else { return false };
        (word, rest) = (w, r);
    }
    basename(&word) == LEGACY_SCRIPT && rest.split_whitespace().all(|a| matches!(a, "alert" | "clear" | "event"))
}

/// Classify one hook command.
pub fn owner(cmd: &str) -> Owner {
    if is_legacy(cmd) {
        return Owner::Legacy;
    }
    let Some((exe, rest)) = first_word(cmd) else { return Owner::Foreign };
    let mut words = rest.split_whitespace();
    if words.next() != Some("hook") {
        return Owner::Foreign;
    }
    let args: Vec<&str> = words.collect();
    if !matches!(args.first(), Some(&"alert" | &"clear" | &"event")) {
        return Owner::Foreign;
    }
    if !basename(&exe).starts_with("clawdmeter") {
        return Owner::Foreign;
    }
    Owner::Ours { exe, args: args.join(" ") }
}

fn command_of(hook: &Value) -> Option<&str> {
    hook.get("command").and_then(Value::as_str)
}

/// A group's matcher; absent, `""` and `"*"` all mean "every tool".
fn group_matcher(group: &Value) -> Option<&str> {
    group.get("matcher").and_then(Value::as_str).filter(|m| !m.is_empty() && *m != "*")
}

fn is_managed(hook: &Value) -> bool {
    command_of(hook).is_some_and(|c| owner(c) != Owner::Foreign)
}

/// settings.json problems.
#[derive(Debug)]
pub enum HookError {
    /// Not valid JSON (left untouched).
    Parse(String),
    /// Valid JSON, unexpected shape (left untouched).
    Shape(&'static str),
    /// File I/O.
    Io(io::Error),
}

impl std::fmt::Display for HookError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            HookError::Parse(_) => {
                write!(f, "settings.json contains invalid JSON. Fix the file, then retry. Nothing was written.")
            }
            HookError::Shape(w) => write!(f, "Unsupported settings.json layout: {w}. Nothing was written."),
            HookError::Io(e) => write!(f, "{}", crate::errors::io_message(e)),
        }
    }
}

impl std::error::Error for HookError {}

impl From<io::Error> for HookError {
    fn from(e: io::Error) -> Self {
        HookError::Io(e)
    }
}

fn parse_root(text: Option<&str>) -> Result<Map<String, Value>, HookError> {
    match text.map(str::trim) {
        None | Some("") => Ok(Map::new()),
        Some(t) => match serde_json::from_str::<Value>(t).map_err(|e| HookError::Parse(e.to_string()))? {
            Value::Object(m) => Ok(m),
            _ => Err(HookError::Shape("the top level isn't an object")),
        },
    }
}

fn render(root: Map<String, Value>) -> String {
    let mut s = serde_json::to_string_pretty(&Value::Object(root)).expect("serializes");
    s.push('\n');
    s
}

/// Merge our hooks into settings.json text (`None` = no file), pointing them
/// at `exe`. Legacy and stale entries are rewritten in place; duplicates of
/// ours are dropped; foreign hooks are untouched.
pub fn merge(text: Option<&str>, exe: &Path) -> Result<String, HookError> {
    let mut root = parse_root(text)?;
    let hooks = root.entry("hooks").or_insert_with(|| Value::Object(Map::new()));
    let hooks = hooks.as_object_mut().ok_or(HookError::Shape("\"hooks\" isn't an object"))?;
    for spec in HOOKS {
        let cmd = hook_command(exe, spec.args);
        let list = hooks.entry(spec.event).or_insert_with(|| Value::Array(Vec::new()));
        let list = list.as_array_mut().ok_or(HookError::Shape("a hook event isn't a list"))?;
        let mut placed = false;
        for group in list.iter_mut() {
            let Some(hs) = group.get("hooks").and_then(Value::as_array) else { continue };
            if !hs.iter().any(is_managed) {
                continue;
            }
            let shared = hs.iter().any(|h| !is_managed(h));
            let matcher_ok = group_matcher(group) == spec.matcher;
            let hs = group["hooks"].as_array_mut().expect("checked");
            if placed || (shared && !matcher_ok) {
                // A duplicate, or sharing a group whose matcher we can't change
                // without affecting someone else's hook: take ours out.
                hs.retain(|h| !is_managed(h));
                continue;
            }
            let mut first = true;
            hs.retain_mut(|h| {
                if !is_managed(h) {
                    return true;
                }
                if !first {
                    return false;
                }
                first = false;
                h["command"] = Value::String(cmd.clone());
                if h.get("type").is_none() {
                    h["type"] = Value::String("command".into());
                }
                true
            });
            // The group is ours alone here (or already matches): make its
            // matcher exactly ours, so e.g. a `"Bash"` matcher left on a
            // PostToolUse group doesn't narrow `clear` to Bash calls.
            if !matcher_ok {
                let g = group.as_object_mut().expect("has hooks, so an object");
                match spec.matcher {
                    Some(m) => g.insert("matcher".into(), Value::String(m.to_owned())),
                    None => g.remove("matcher"),
                };
            }
            placed = true;
        }
        list.retain(|g| g.get("hooks").and_then(Value::as_array).is_none_or(|hs| !hs.is_empty()));
        if !placed {
            let mut g = Map::new();
            if let Some(m) = spec.matcher {
                g.insert("matcher".into(), Value::String(m.to_owned()));
            }
            g.insert("hooks".into(), json!([{ "type": "command", "command": cmd }]));
            list.push(Value::Object(g));
        }
    }
    // Legacy entries on events we no longer use.
    strip_managed(hooks, |event| !HOOKS.iter().any(|s| s.event == event));
    Ok(render(root))
}

fn strip_managed(hooks: &mut Map<String, Value>, which: impl Fn(&str) -> bool) {
    for (event, list) in hooks.iter_mut() {
        if !which(event) {
            continue;
        }
        if let Value::Array(groups) = list {
            for g in groups.iter_mut() {
                if let Some(Value::Array(hs)) = g.get_mut("hooks") {
                    hs.retain(|h| !is_managed(h));
                }
            }
            groups.retain(|g| g.get("hooks").and_then(Value::as_array).is_none_or(|hs| !hs.is_empty()));
        }
    }
    hooks.retain(|_, v| v.as_array().is_none_or(|a| !a.is_empty()));
}

/// Remove our hook entries (and legacy ones), plus groups and events left
/// empty by that.
pub fn remove(text: Option<&str>) -> Result<String, HookError> {
    let mut root = parse_root(text)?;
    if let Some(Value::Object(hooks)) = root.get_mut("hooks") {
        strip_managed(hooks, |_| true);
        if hooks.is_empty() {
            root.remove("hooks");
        }
    }
    Ok(render(root))
}

/// How one settings.json stands.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum HookStatus {
    /// None of our hooks.
    Missing,
    /// All five, pointing at this binary.
    Installed,
    /// Uses the old `attention-hook.sh` script.
    Legacy,
    /// Points at a different (moved, removed or older) Clawdmeter binary.
    Stale(String),
    /// Some hooks missing or with old arguments.
    Partial,
    /// settings.json can't be read or parsed.
    Unreadable(String),
}

impl HookStatus {
    /// Applying the installer would change something.
    pub fn needs_install(&self) -> bool {
        !matches!(self, HookStatus::Installed | HookStatus::Unreadable(_))
    }
}

/// Check a settings.json against [`HOOKS`] and `exe`.
pub fn status(text: Option<&str>, exe: &Path) -> HookStatus {
    let root = match parse_root(text) {
        Ok(r) => r,
        Err(e) => return HookStatus::Unreadable(e.to_string()),
    };
    let hooks = root.get("hooks").and_then(Value::as_object);
    let all: Vec<Owner> = hooks
        .into_iter()
        .flat_map(|h| h.values())
        .filter_map(Value::as_array)
        .flatten()
        .filter_map(|g| g.get("hooks").and_then(Value::as_array))
        .flatten()
        .filter_map(command_of)
        .map(owner)
        .filter(|o| *o != Owner::Foreign)
        .collect();
    if all.is_empty() {
        return HookStatus::Missing;
    }
    if all.contains(&Owner::Legacy) {
        return HookStatus::Legacy;
    }
    let want = quote_exe(exe);
    // Another binary, or one that won't be there later (a disk image, a zip).
    let stale = |e: &str| quote_exe(Path::new(e)) != want || clawdmeter_core::launch::unstable(Path::new(e)).is_some();
    if let Some(Owner::Ours { exe: other, .. }) =
        all.iter().find(|o| matches!(o, Owner::Ours { exe: e, .. } if stale(e)))
    {
        return HookStatus::Stale(other.clone());
    }
    let complete = HOOKS.iter().all(|spec| {
        hooks
            .and_then(|h| h.get(spec.event))
            .and_then(Value::as_array)
            .into_iter()
            .flatten()
            .filter(|g| group_matcher(g) == spec.matcher)
            .filter_map(|g| g.get("hooks").and_then(Value::as_array))
            .flatten()
            .filter_map(command_of)
            .any(|c| matches!(owner(c), Owner::Ours { args, .. } if args == spec.args))
    });
    if complete {
        HookStatus::Installed
    } else {
        HookStatus::Partial
    }
}

/// What a [`HookPlan`] does, so it can be redone on a file that changed.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum HookOp {
    /// Install (or update) the hooks for this executable.
    Install(PathBuf),
    /// Remove them.
    Remove,
}

impl HookOp {
    fn run(&self, text: Option<&str>) -> Result<String, HookError> {
        match self {
            HookOp::Install(exe) => merge(text, exe),
            HookOp::Remove => remove(text),
        }
    }
}

/// A pending change to one account's settings.json, for the diff view.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct HookPlan {
    /// `<config_dir>/settings.json`.
    pub path: PathBuf,
    /// Current text (`None` = no file).
    pub before: Option<String>,
    /// Text after the change.
    pub after: String,
    /// The change, redone at [`HookPlan::apply`] if the file moved on.
    pub op: HookOp,
}

impl HookPlan {
    /// Applying would change anything (ignoring formatting).
    pub fn changes(&self) -> bool {
        let parse = |s: &str| serde_json::from_str::<Value>(s).ok();
        match &self.before {
            None => true,
            Some(b) => parse(b) != parse(&self.after),
        }
    }

    /// Write it. The file is read again first: if Claude Code (or anyone)
    /// changed it since the preview, the same change is made to the new text,
    /// so their edits survive. The first file ever replaced is kept as
    /// `settings.json.bak` (the settings from before Clawdmeter); later
    /// writes leave that backup alone.
    pub fn apply(&self) -> Result<(), HookError> {
        let now = read_opt(&self.path)?;
        let plan = if now == self.before {
            self.clone()
        } else {
            HookPlan { after: self.op.run(now.as_deref())?, before: now, ..self.clone() }
        };
        if !plan.changes() {
            return Ok(());
        }
        if let Some(dir) = plan.path.parent() {
            fs::create_dir_all(dir)?;
        }
        let bak = plan.path.with_extension("json.bak");
        if let (Some(b), false) = (&plan.before, bak.exists()) {
            fs::write(&bak, b)?;
        }
        let after = &plan.after;
        // A settings.json symlinked from a dotfiles repo stays a symlink: the
        // new text replaces the file it points at.
        let target = match fs::symlink_metadata(&self.path) {
            Ok(m) if m.file_type().is_symlink() => fs::canonicalize(&self.path)?,
            _ => self.path.clone(),
        };
        let tmp = target.with_extension("json.tmp");
        fs::write(&tmp, after)?;
        Ok(fs::rename(&tmp, &target)?)
    }
}

fn read_opt(path: &Path) -> Result<Option<String>, HookError> {
    match fs::read_to_string(path) {
        Ok(s) => Ok(Some(s)),
        Err(e) if e.kind() == io::ErrorKind::NotFound => Ok(None),
        Err(e) => Err(e.into()),
    }
}

/// Plan installing (or updating) the hooks in one config dir.
pub fn plan_install(config_dir: &Path, exe: &Path) -> Result<HookPlan, HookError> {
    let path = config_dir.join("settings.json");
    let before = read_opt(&path)?;
    let op = HookOp::Install(exe.to_owned());
    let after = op.run(before.as_deref())?;
    Ok(HookPlan { path, before, after, op })
}

/// Plan removing the hooks from one config dir.
pub fn plan_uninstall(config_dir: &Path) -> Result<HookPlan, HookError> {
    let path = config_dir.join("settings.json");
    let before = read_opt(&path)?;
    let op = HookOp::Remove;
    let after = op.run(before.as_deref())?;
    Ok(HookPlan { path, before, after, op })
}

/// Hook status of one config dir.
pub fn dir_status(config_dir: &Path, exe: &Path) -> HookStatus {
    match read_opt(&config_dir.join("settings.json")) {
        Ok(t) => status(t.as_deref(), exe),
        Err(e) => HookStatus::Unreadable(e.to_string()),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const EXE: &str = "/opt/Clawdmeter/clawdmeter";

    fn exe() -> &'static Path {
        Path::new(EXE)
    }

    fn v(s: &str) -> Value {
        serde_json::from_str(s).unwrap()
    }

    #[test]
    #[cfg(unix)]
    fn commands_and_quoting() {
        assert_eq!(hook_command(exe(), "clear event"), "/opt/Clawdmeter/clawdmeter hook clear event");
        let spaced = hook_command(Path::new("/home/u/My Apps/clawdmeter"), "event");
        assert_eq!(spaced, "'/home/u/My Apps/clawdmeter' hook event");
        assert_eq!(hook_command(Path::new("/x/it's/clawdmeter"), "event"), r"'/x/it'\''s/clawdmeter' hook event");
        // What we write, we recognise.
        for p in ["/opt/Clawdmeter/clawdmeter", "/home/u/My Apps/clawdmeter", "/x/it's/clawdmeter"] {
            assert_eq!(
                owner(&hook_command(Path::new(p), "alert event")),
                Owner::Ours { exe: p.into(), args: "alert event".into() }
            );
        }
    }

    #[test]
    fn ownership() {
        assert_eq!(owner("/home/u/Clawdmeter/daemon/attention-hook.sh clear"), Owner::Legacy);
        assert_eq!(
            owner("\"C:/Program Files/Clawdmeter/clawdmeter.exe\" hook alert event"),
            Owner::Ours { exe: "C:/Program Files/Clawdmeter/clawdmeter.exe".into(), args: "alert event".into() }
        );
        assert_eq!(
            owner(r#""C:\Users\u\clawdmeter.exe" hook event"#),
            Owner::Ours { exe: r"C:\Users\u\clawdmeter.exe".into(), args: "event".into() }
        );
        assert_eq!(
            owner("/apps/Clawdmeter-x86_64.AppImage hook event"),
            Owner::Ours { exe: "/apps/Clawdmeter-x86_64.AppImage".into(), args: "event".into() }
        );
        assert_eq!(owner("sh '/home/u/My Code/daemon/attention-hook.sh' alert event"), Owner::Legacy);
        assert_eq!(owner("bash ~/attention-hook.sh clear"), Owner::Legacy);
        for foreign in [
            "prettier --write",
            "/usr/bin/other hook event",
            "/x/clawdmeter --daemon",
            "/x/clawdmeter hook",
            "",
            "/x/attention-hook.sh alert && notify-send hi",
            "my-wrapper /x/attention-hook.sh alert",
            "cat /x/attention-hook.sh.bak",
        ] {
            assert_eq!(owner(foreign), Owner::Foreign, "{foreign:?}");
        }
    }

    #[test]
    fn fresh_install() {
        let out = merge(None, exe()).unwrap();
        let j = v(&out);
        assert_eq!(j["hooks"]["Stop"][0]["hooks"][0]["command"], format!("{EXE} hook event"));
        assert_eq!(j["hooks"]["PreToolUse"][0]["matcher"], "AskUserQuestion");
        assert_eq!(j["hooks"]["PostToolUse"][0]["hooks"][0]["type"], "command");
        assert_eq!(status(Some(&out), exe()), HookStatus::Installed);
        assert_eq!(status(None, exe()), HookStatus::Missing);
        assert_eq!(merge(Some(&out), exe()).unwrap(), out, "idempotent");
    }

    #[test]
    fn migrates_legacy_entries_in_place() {
        // The old README's manual setup (alert/clear only) plus a user hook.
        let before = r#"{
  "model": "opus",
  "hooks": {
    "PostToolUse": [
      {"matcher": "Edit", "hooks": [{"type": "command", "command": "prettier --write"}]},
      {"hooks": [{"type": "command", "command": "/home/u/Clawdmeter/daemon/attention-hook.sh clear"}]}
    ],
    "Notification": [{"hooks": [{"type": "command", "command": "~/Clawdmeter/daemon/attention-hook.sh alert"}]}],
    "PreToolUse": [{"matcher": "AskUserQuestion", "hooks": [{"type": "command", "command": "/home/u/.config/claude-usage-monitor/attention-hook.sh alert event"}]}]
  }
}"#;
        assert_eq!(status(Some(before), exe()), HookStatus::Legacy);
        let out = merge(Some(before), exe()).unwrap();
        let j = v(&out);
        let keys: Vec<_> = j.as_object().unwrap().keys().cloned().collect();
        assert_eq!(keys, ["model", "hooks"]);
        let post = j["hooks"]["PostToolUse"].as_array().unwrap();
        assert_eq!(post.len(), 2, "migrated in place, no new group");
        assert_eq!(post[0]["hooks"][0]["command"], "prettier --write");
        assert_eq!(post[1]["hooks"][0]["command"], format!("{EXE} hook clear event"));
        assert_eq!(j["hooks"]["Notification"].as_array().unwrap().len(), 1);
        assert_eq!(j["hooks"]["PreToolUse"][0]["hooks"][0]["command"], format!("{EXE} hook alert event"));
        assert!(!out.contains(LEGACY_SCRIPT));
        assert_eq!(status(Some(&out), exe()), HookStatus::Installed);
    }

    #[test]
    fn stale_paths_and_duplicates() {
        let old = merge(None, Path::new("/old/place/clawdmeter")).unwrap();
        assert_eq!(status(Some(&old), exe()), HookStatus::Stale("/old/place/clawdmeter".into()));
        let fixed = merge(Some(&old), exe()).unwrap();
        assert_eq!(status(Some(&fixed), exe()), HookStatus::Installed);
        assert!(!fixed.contains("/old/place"));
        // Both a legacy and a current entry on one event: one survives.
        let dup = format!(
            r#"{{"hooks": {{"Stop": [
              {{"hooks": [{{"type": "command", "command": "/x/attention-hook.sh event"}}]}},
              {{"hooks": [{{"type": "command", "command": "{EXE} hook event"}}, {{"type": "command", "command": "say done"}}]}}]}}}}"#
        );
        let j = v(&merge(Some(&dup), exe()).unwrap());
        let stop = j["hooks"]["Stop"].as_array().unwrap();
        assert_eq!(stop.len(), 2);
        assert_eq!(stop[0]["hooks"][0]["command"], format!("{EXE} hook event"));
        assert_eq!(stop[1]["hooks"].as_array().unwrap().len(), 1);
        assert_eq!(stop[1]["hooks"][0]["command"], "say done");
    }

    #[test]
    fn never_retargets_someone_elses_group() {
        // Our hook shares a matcher-less PreToolUse group with a user hook:
        // setting the matcher would narrow theirs, so ours moves out.
        let before = r#"{"hooks": {"PreToolUse": [{"hooks": [
            {"type": "command", "command": "audit-log"},
            {"type": "command", "command": "/x/attention-hook.sh alert"}]}]}}"#;
        let j = v(&merge(Some(before), exe()).unwrap());
        let pre = j["hooks"]["PreToolUse"].as_array().unwrap();
        assert_eq!(pre.len(), 2);
        assert!(pre[0].get("matcher").is_none());
        assert_eq!(pre[0]["hooks"].as_array().unwrap().len(), 1);
        assert_eq!(pre[0]["hooks"][0]["command"], "audit-log");
        assert_eq!(pre[1]["matcher"], "AskUserQuestion");
    }

    #[test]
    fn matcher_left_on_a_group_is_fixed_or_avoided() {
        // Legacy clear alone in a "Bash" group: the group becomes catch-all.
        let alone = r#"{"hooks": {"PostToolUse": [{"matcher": "Bash", "hooks": [
            {"type": "command", "command": "/x/attention-hook.sh clear"}]}]}}"#;
        let j = v(&merge(Some(alone), exe()).unwrap());
        let post = j["hooks"]["PostToolUse"].as_array().unwrap();
        assert_eq!(post.len(), 1);
        assert!(post[0].get("matcher").is_none());
        // Shared with a user's Bash hook: theirs keeps its matcher, ours moves.
        let shared = r#"{"hooks": {"PostToolUse": [{"matcher": "Bash", "hooks": [
            {"type": "command", "command": "lint"},
            {"type": "command", "command": "/x/attention-hook.sh clear"}]}]}}"#;
        assert_eq!(status(Some(shared), exe()), HookStatus::Legacy);
        let out = merge(Some(shared), exe()).unwrap();
        let j = v(&out);
        let post = j["hooks"]["PostToolUse"].as_array().unwrap();
        assert_eq!(post.len(), 2);
        assert_eq!(post[0]["matcher"], "Bash");
        assert_eq!(post[0]["hooks"].as_array().unwrap().len(), 1);
        assert!(post[1].get("matcher").is_none());
        assert_eq!(status(Some(&out), exe()), HookStatus::Installed);
        // "*" counts as catch-all and is left alone.
        let star = format!(
            r#"{{"hooks": {{"Stop": [{{"matcher": "*", "hooks": [{{"type": "command", "command": "{EXE} hook event"}}]}}]}}}}"#
        );
        assert_eq!(v(&merge(Some(&star), exe()).unwrap())["hooks"]["Stop"][0]["matcher"], "*");
    }

    #[test]
    fn remove_only_ours() {
        let before = r#"{"hooks": {"PostToolUse": [
            {"matcher": "Edit", "hooks": [{"type": "command", "command": "prettier --write"}]},
            {"hooks": [{"type": "command", "command": "/x/attention-hook.sh clear event"}]}],
          "Stop": [{"hooks": [{"type": "command", "command": "/opt/clawdmeter hook event"}]}],
          "SessionStart": [{"hooks": [{"type": "command", "command": "echo hi"}]}]}}"#;
        let j = v(&remove(Some(before)).unwrap());
        assert_eq!(j["hooks"]["PostToolUse"].as_array().unwrap().len(), 1);
        assert!(j["hooks"].get("Stop").is_none());
        assert_eq!(j["hooks"]["SessionStart"][0]["hooks"][0]["command"], "echo hi");
        let only_ours =
            r#"{"a": 1, "hooks": {"Stop": [{"hooks": [{"type": "command", "command": "/x/clawdmeter hook event"}]}]}}"#;
        assert_eq!(remove(Some(only_ours)).unwrap(), "{\n  \"a\": 1\n}\n");
    }

    #[test]
    fn partial_and_unreadable() {
        let partial = format!(
            r#"{{"hooks": {{"Stop": [{{"hooks": [{{"type": "command", "command": "{EXE} hook event"}}]}}]}}}}"#
        );
        assert_eq!(status(Some(&partial), exe()), HookStatus::Partial);
        assert!(matches!(status(Some("{not json"), exe()), HookStatus::Unreadable(_)));
        assert!(matches!(merge(Some("{not json"), exe()), Err(HookError::Parse(_))));
        assert!(matches!(merge(Some("[]"), exe()), Err(HookError::Shape(_))));
        assert!(matches!(merge(Some(r#"{"hooks": []}"#), exe()), Err(HookError::Shape(_))));
    }

    #[test]
    fn apply_keeps_edits_made_after_the_preview() {
        let dir = std::env::temp_dir().join(format!("cm-hookstale-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        let path = dir.join("settings.json");
        fs::write(&path, "{\"model\":\"opus\"}").unwrap();
        let plan = plan_install(&dir, exe()).unwrap();
        // Claude Code writes the file while the preview is open.
        fs::write(&path, "{\"model\":\"opus\",\"theme\":\"dark\"}").unwrap();
        plan.apply().unwrap();
        let v: Value = serde_json::from_str(&fs::read_to_string(&path).unwrap()).unwrap();
        assert_eq!(v["theme"], "dark");
        assert_eq!(dir_status(&dir, exe()), HookStatus::Installed);
        // Broken in the meantime: nothing is written.
        let plan = plan_uninstall(&dir).unwrap();
        fs::write(&path, "{ not json").unwrap();
        assert!(matches!(plan.apply(), Err(HookError::Parse(_))));
        assert_eq!(fs::read_to_string(&path).unwrap(), "{ not json");
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn plan_apply_with_backup() {
        let dir = std::env::temp_dir().join(format!("cm-hookinst-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        fs::write(dir.join("settings.json"), "{\"model\":\"opus\"}").unwrap();
        let plan = plan_install(&dir, exe()).unwrap();
        assert!(plan.changes());
        plan.apply().unwrap();
        assert_eq!(fs::read_to_string(dir.join("settings.json.bak")).unwrap(), "{\"model\":\"opus\"}");
        assert_eq!(dir_status(&dir, exe()), HookStatus::Installed);
        assert!(!plan_install(&dir, exe()).unwrap().changes());
        plan_uninstall(&dir).unwrap().apply().unwrap();
        assert_eq!(dir_status(&dir, exe()), HookStatus::Missing);
        assert_eq!(
            fs::read_to_string(dir.join("settings.json.bak")).unwrap(),
            "{\"model\":\"opus\"}",
            "the backup keeps the settings from before Clawdmeter"
        );
        #[cfg(unix)]
        {
            // A symlinked settings.json keeps its link; the target changes.
            let real = dir.join("dotfiles-settings.json");
            fs::rename(dir.join("settings.json"), &real).unwrap();
            std::os::unix::fs::symlink(&real, dir.join("settings.json")).unwrap();
            plan_install(&dir, exe()).unwrap().apply().unwrap();
            assert!(fs::symlink_metadata(dir.join("settings.json")).unwrap().file_type().is_symlink());
            assert_eq!(status(Some(&fs::read_to_string(&real).unwrap()), exe()), HookStatus::Installed);
        }
        let _ = fs::remove_dir_all(&dir);
    }
}
