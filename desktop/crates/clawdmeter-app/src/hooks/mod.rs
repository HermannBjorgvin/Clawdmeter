//! Claude Code hooks: `clawdmeter hook <alert|clear|event> [event]` (the hook
//! itself, see [`run`]) and the installer that wires it into each account's
//! `settings.json` ([`install`]).
//!
//! The hook writes the files the daemon follows, under the state dir
//! (`~/.config/clawdmeter`, `%LOCALAPPDATA%\Clawdmeter` on Windows):
//!
//! * `alert` writes `attention` (`{"k": kind, "m": message, "s": session
//!   id}`, one line; the kind lets the daemon drop alert types the user
//!   turned off, see [`clawdmeter_core::attention::AlertKind`]);
//! * `clear` touches `clear-<session id>`;
//! * `event` appends `<unix_ts> <hook_event_name> <session_id>` to
//!   `events.log`, rolling it to `events.log.1` past 256 KB.
//!
//! `alert event` / `clear event` do both from one read of stdin. Only the
//! event name and session id are recorded, never the prompt or the tool call.
//! Hook stdout can reach the model, so the hook prints nothing and always
//! exits 0. The file formats match the legacy `attention-hook.sh` byte for
//! byte apart from the alert's `k`, which older readers ignore.

pub mod install;

use std::fs::{self, File, OpenOptions};
use std::io::{self, Write};
use std::path::Path;
use std::time::SystemTime;

use clawdmeter_core::attention::AlertKind;
use serde_json::{Map, Value};

/// `events.log` rolls over once it is larger than this.
pub const EVENTS_ROLL_BYTES: u64 = 262_144;

/// What one invocation does.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Mode {
    /// The first argument: `alert`, `clear` or `event` (anything else is a
    /// no-op, like the script's `case`).
    pub action: Action,
    /// The second argument was `event`: also log the event.
    pub log_too: bool,
}

/// The first argument.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Action {
    /// Write the attention file.
    Alert,
    /// Touch `clear-<sid>`.
    Clear,
    /// Append to events.log.
    Event,
    /// Unknown or missing: do nothing (still exit 0).
    None,
}

impl Mode {
    /// Parse the arguments after `hook`.
    pub fn parse(args: &[String]) -> Self {
        let action = match args.first().map(String::as_str) {
            Some("alert") => Action::Alert,
            Some("clear") => Action::Clear,
            Some("event") => Action::Event,
            _ => Action::None,
        };
        Mode { action, log_too: args.get(1).map(String::as_str) == Some("event") }
    }
}

/// Run the hook: read `input` (the hook JSON), write under `state_dir`.
/// Errors are swallowed; the caller exits 0 regardless.
pub fn run(mode: Mode, input: &str, state_dir: &Path, now_unix: u64) {
    if fs::create_dir_all(state_dir).is_err() {
        return;
    }
    // Invalid JSON: the script's jq calls all fail, so nothing is written.
    let Ok(v) = serde_json::from_str::<Value>(input) else { return };
    if mode.log_too {
        let _ = log_event(&v, state_dir, now_unix);
    }
    let _ = match mode.action {
        Action::Alert => write_alert(&v, state_dir),
        Action::Clear => touch_clear(&v, state_dir),
        Action::Event => log_event(&v, state_dir, now_unix),
        Action::None => Ok(()),
    };
}

/// jq's `//`: the value unless it is missing, null or false.
fn present(v: Option<&Value>) -> Option<&Value> {
    v.filter(|v| !matches!(v, Value::Null | Value::Bool(false)))
}

/// jq's `tostring`: strings verbatim, everything else as compact JSON.
fn jq_tostring(v: &Value) -> String {
    match v {
        Value::String(s) => s.clone(),
        other => other.to_string(),
    }
}

fn sanitize(s: &str) -> String {
    s.chars().filter(|c| c.is_ascii_alphanumeric() || matches!(c, '_' | '.' | '-')).collect()
}

/// jq field access on the whole input: an object, or `null` (where every
/// field is null); anything else is a jq error.
fn as_jq_object(v: &Value) -> Option<std::borrow::Cow<'_, Map<String, Value>>> {
    match v {
        Value::Object(m) => Some(std::borrow::Cow::Borrowed(m)),
        Value::Null => Some(std::borrow::Cow::Owned(Map::new())),
        _ => None,
    }
}

/// The Claude Code config dir (account) a hook input comes from: the
/// directory holding `projects/` in its `transcript_path`
/// (`<config dir>/projects/<project>/<session>.jsonl`).
pub fn account_of(v: &Value) -> Option<String> {
    let path = Path::new(v.get("transcript_path")?.as_str()?);
    let dir = path.ancestors().find(|a| a.file_name().is_some_and(|n| n == "projects"))?.parent()?;
    let s = dir.to_str()?;
    (!s.is_empty() && !s.chars().any(char::is_control)).then(|| s.to_owned())
}

/// `<ts> <event> <session> [<config dir>]`: event and session reduced to
/// `[A-Za-z0-9_.-]`, the config dir ([`account_of`]) as the rest of the line
/// when the input has a transcript path.
pub fn event_line(v: &Value, now_unix: u64) -> Option<String> {
    // `.hook_event_name` on a non-object is a jq error: no line.
    let obj = as_jq_object(v)?;
    let name = present(obj.get("hook_event_name")).map_or_else(|| "unknown".to_owned(), jq_tostring);
    let sid = present(obj.get("session_id")).map_or_else(|| "-".to_owned(), jq_tostring);
    let (name, sid) = (sanitize(&name), sanitize(&sid));
    Some(match account_of(v) {
        Some(acct) => format!("{now_unix} {name} {} {acct}\n", if sid.is_empty() { "-" } else { &sid }),
        None => format!("{now_unix} {name} {sid}\n"),
    })
}

fn log_event(v: &Value, dir: &Path, now_unix: u64) -> io::Result<()> {
    let Some(line) = event_line(v, now_unix) else { return Ok(()) };
    let f = dir.join("events.log");
    if fs::metadata(&f).is_ok_and(|m| m.is_file() && m.len() > EVENTS_ROLL_BYTES) {
        let _ = fs::rename(&f, dir.join("events.log.1"));
    }
    // One write call: concurrent hooks append whole lines (O_APPEND).
    OpenOptions::new().create(true).append(true).open(&f)?.write_all(line.as_bytes())
}

/// The attention file's text: jq -c '{m: (.message // .tool_input.questions[0].question // ""), s: .session_id}',
/// plus `k`, the alert kind.
pub fn alert_json(v: &Value) -> Option<String> {
    let obj = as_jq_object(v)?;
    let m = match present(obj.get("message")) {
        Some(m) => m.clone(),
        // jq raises (and the script writes nothing) when a step along
        // `.tool_input.questions[0].question` has the wrong type.
        None => {
            present(question(obj.get("tool_input"))?.as_ref()).cloned().unwrap_or_else(|| Value::String(String::new()))
        }
    };
    let s = obj.get("session_id").cloned().unwrap_or(Value::Null);
    let text = |k: &str| obj.get(k).and_then(Value::as_str);
    let kind = AlertKind::from_hook(text("hook_event_name"), text("notification_type"));
    let mut out = Map::new();
    out.insert("k".into(), Value::String(kind.as_str().into()));
    out.insert("m".into(), m);
    out.insert("s".into(), s);
    // jq escapes DEL; serde_json writes it raw. DEL only occurs inside strings.
    Some(Value::Object(out).to_string().replace('\u{7f}', "\\u007f") + "\n")
}

/// `.tool_input.questions[0].question` with jq's typing: indexing null gives
/// null, indexing the wrong type is an error (`None`).
fn question(tool_input: Option<&Value>) -> Option<Option<Value>> {
    fn key(v: Option<&Value>, k: &str) -> Option<Option<Value>> {
        match v {
            None | Some(Value::Null) => Some(None),
            Some(Value::Object(m)) => Some(m.get(k).cloned()),
            Some(_) => None,
        }
    }
    let questions = key(tool_input, "questions")?;
    let first = match &questions {
        None | Some(Value::Null) => None,
        Some(Value::Array(a)) => a.first().cloned(),
        Some(_) => return None,
    };
    key(first.as_ref(), "question")
}

fn write_alert(v: &Value, dir: &Path) -> io::Result<()> {
    let Some(body) = alert_json(v) else { return Ok(()) };
    let tmp = dir.join(format!("attention.{}", std::process::id()));
    fs::write(&tmp, body)?;
    fs::rename(&tmp, dir.join("attention")).inspect_err(|_| {
        let _ = fs::remove_file(&tmp);
    })
}

/// The `clear-<sid>` file name, or `None` when there is no usable session id.
/// Ids that would leave the state dir (`/`, `\`, `..`) are refused.
pub fn clear_name(v: &Value) -> Option<String> {
    let sid = match present(v.as_object()?.get("session_id"))? {
        Value::String(s) => s.clone(),
        n @ Value::Number(_) => n.to_string(),
        Value::Bool(true) => "true".into(),
        _ => return None,
    };
    let unsafe_id = sid.is_empty() || sid == "." || sid == ".." || sid.contains(['/', '\\', '\0', '\n']);
    (!unsafe_id).then(|| format!("clear-{sid}"))
}

fn touch_clear(v: &Value, dir: &Path) -> io::Result<()> {
    let Some(name) = clear_name(v) else { return Ok(()) };
    let f: File = OpenOptions::new().create(true).truncate(false).write(true).open(dir.join(name))?;
    // `touch` on an existing file: the daemon compares this mtime with the alert's.
    f.set_modified(SystemTime::now())
}

/// Entry point for `clawdmeter hook …`: read stdin, run, never fail.
pub fn main(args: &[String]) -> i32 {
    use std::io::{IsTerminal as _, Read as _};
    let mode = Mode::parse(args);
    let mut input = String::new();
    // Read stdin even for a no-op mode, so Claude Code never sees a broken
    // pipe; but never wait on a terminal (someone trying it by hand).
    let stdin = io::stdin();
    if !stdin.is_terminal() {
        let _ = stdin.lock().read_to_string(&mut input);
    }
    if mode.action != Action::None || mode.log_too {
        let state = clawdmeter_core::Paths::for_user().state_dir;
        let now = SystemTime::now().duration_since(SystemTime::UNIX_EPOCH).map_or(0, |d| d.as_secs());
        run(mode, &input, &state, now);
    }
    0
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn dir(tag: &str) -> std::path::PathBuf {
        let d = std::env::temp_dir().join(format!("cm-hook-{tag}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&d);
        d
    }

    fn args(s: &str) -> Vec<String> {
        s.split_whitespace().map(str::to_owned).collect()
    }

    #[test]
    fn mode_grammar() {
        assert_eq!(Mode::parse(&args("alert")), Mode { action: Action::Alert, log_too: false });
        assert_eq!(Mode::parse(&args("clear event")), Mode { action: Action::Clear, log_too: true });
        assert_eq!(Mode::parse(&args("event")), Mode { action: Action::Event, log_too: false });
        assert_eq!(Mode::parse(&args("bogus event")), Mode { action: Action::None, log_too: true });
        assert_eq!(Mode::parse(&[]), Mode { action: Action::None, log_too: false });
    }

    #[test]
    fn event_lines() {
        let v = json!({"hook_event_name": "Stop", "session_id": "ab-12.x_y", "prompt": "secret"});
        assert_eq!(event_line(&v, 7).unwrap(), "7 Stop ab-12.x_y\n");
        assert_eq!(event_line(&json!({}), 7).unwrap(), "7 unknown -\n");
        assert_eq!(event_line(&json!({"session_id": ""}), 7).unwrap(), "7 unknown \n");
        assert_eq!(
            event_line(&json!({"hook_event_name": "Pré Tool$Use", "session_id": 42}), 7).unwrap(),
            "7 PrToolUse 42\n"
        );
        assert_eq!(event_line(&json!({"session_id": false, "hook_event_name": null}), 7).unwrap(), "7 unknown -\n");
        assert_eq!(event_line(&json!({"session_id": {"a": [1, "b"]}}), 7).unwrap(), "7 unknown a1b\n");
        assert!(event_line(&json!([1]), 7).is_none());
        assert_eq!(event_line(&Value::Null, 7).unwrap(), "7 unknown -\n");
        // The account comes from the transcript path.
        let v = json!({"hook_event_name": "UserPromptSubmit", "session_id": "s",
            "transcript_path": "/home/me/My Claude/projects/-home-me-code/s.jsonl"});
        assert_eq!(event_line(&v, 7).unwrap(), "7 UserPromptSubmit s /home/me/My Claude\n");
        let parsed = clawdmeter_core::events::parse_line(event_line(&v, 7).unwrap().trim_end()).unwrap();
        assert_eq!(parsed.account.as_deref(), Some(Path::new("/home/me/My Claude")));
        let v = json!({"session_id": "", "transcript_path": "/h/.claude/projects/p/x.jsonl"});
        assert_eq!(event_line(&v, 7).unwrap(), "7 unknown - /h/.claude\n");
        assert_eq!(account_of(&json!({"transcript_path": "/tmp/x.jsonl"})), None);
        assert_eq!(account_of(&json!({"transcript_path": "/h/pro\njects/projects/p/x.jsonl"})), None);
    }

    #[test]
    fn alert_bodies() {
        let n = json!({"message": "Claude needs your permission", "session_id": "s1"});
        assert_eq!(
            alert_json(&n).unwrap(),
            "{\"k\":\"permission\",\"m\":\"Claude needs your permission\",\"s\":\"s1\"}\n"
        );
        let q = json!({"hook_event_name": "PreToolUse", "tool_input": {"questions": [{"question": "Pick one"}]}, "session_id": "s2"});
        assert_eq!(alert_json(&q).unwrap(), "{\"k\":\"question\",\"m\":\"Pick one\",\"s\":\"s2\"}\n");
        let idle = json!({"hook_event_name": "Notification", "notification_type": "idle_prompt", "message": "Waiting"});
        assert_eq!(alert_json(&idle).unwrap(), "{\"k\":\"idle\",\"m\":\"Waiting\",\"s\":null}\n");
        assert_eq!(alert_json(&json!({"tool_input": {}})).unwrap(), "{\"k\":\"permission\",\"m\":\"\",\"s\":null}\n");
        assert_eq!(
            alert_json(&json!({"tool_input": {"questions": []}})).unwrap(),
            "{\"k\":\"permission\",\"m\":\"\",\"s\":null}\n"
        );
        // jq errors on these, so the script writes nothing.
        assert_eq!(alert_json(&json!({"tool_input": "x"})), None);
        assert_eq!(alert_json(&json!({"tool_input": {"questions": {"a": 1}}})), None);
        assert_eq!(
            alert_json(&json!({"message": "wins", "tool_input": "x"})).unwrap(),
            "{\"k\":\"permission\",\"m\":\"wins\",\"s\":null}\n"
        );
        assert_eq!(
            alert_json(&json!({"message": "a\u{7f}\"b"})).unwrap(),
            "{\"k\":\"permission\",\"m\":\"a\\u007f\\\"b\",\"s\":null}\n"
        );
    }

    #[test]
    fn clear_names() {
        assert_eq!(clear_name(&json!({"session_id": "abc"})).as_deref(), Some("clear-abc"));
        assert_eq!(clear_name(&json!({"session_id": 5})).as_deref(), Some("clear-5"));
        for bad in [json!({}), json!({"session_id": ""}), json!({"session_id": "../x"}), json!({"session_id": ".."})] {
            assert_eq!(clear_name(&bad), None, "{bad}");
        }
    }

    #[test]
    fn writes_files_and_rolls_the_log() {
        let d = dir("files");
        let input = r#"{"hook_event_name":"Notification","session_id":"s1","message":"Waiting"}"#;
        run(Mode::parse(&args("alert event")), input, &d, 100);
        assert_eq!(
            fs::read_to_string(d.join("attention")).unwrap(),
            "{\"k\":\"permission\",\"m\":\"Waiting\",\"s\":\"s1\"}\n"
        );
        assert_eq!(fs::read_to_string(d.join("events.log")).unwrap(), "100 Notification s1\n");
        run(Mode::parse(&args("clear event")), r#"{"hook_event_name":"PostToolUse","session_id":"s1"}"#, &d, 101);
        assert!(d.join("clear-s1").is_file());
        assert_eq!(fs::read_to_string(d.join("events.log")).unwrap().lines().count(), 2);
        // Garbage in, nothing out.
        run(Mode::parse(&args("alert event")), "not json", &d, 102);
        assert_eq!(fs::read_to_string(d.join("events.log")).unwrap().lines().count(), 2);
        // Rollover past 256 KB.
        fs::write(d.join("events.log"), vec![b'x'; EVENTS_ROLL_BYTES as usize + 1]).unwrap();
        run(Mode::parse(&args("event")), r#"{"hook_event_name":"Stop","session_id":"s1"}"#, &d, 103);
        assert_eq!(fs::read_to_string(d.join("events.log")).unwrap(), "103 Stop s1\n");
        assert_eq!(fs::metadata(d.join("events.log.1")).unwrap().len(), EVENTS_ROLL_BYTES + 1);
        assert!(fs::read_dir(&d)
            .unwrap()
            .flatten()
            .all(|e| !e.file_name().to_string_lossy().starts_with("attention.")));
        let _ = fs::remove_dir_all(&d);
    }

    /// Runs the legacy `attention-hook.sh` (kept verbatim in `testdata/` as
    /// the reference; skipped when `jq` is not installed) and this module on
    /// the same inputs, and compares every file both write. Timestamps are normalized.
    #[test]
    #[cfg(unix)]
    fn matches_the_legacy_script() {
        let script = Path::new(env!("CARGO_MANIFEST_DIR")).join("src/hooks/testdata/attention-hook.sh");
        let has_jq = std::process::Command::new("jq").arg("--version").output().is_ok();
        if !script.is_file() || !has_jq {
            eprintln!("skipped: needs jq");
            return;
        }
        let cases: &[(&str, &str)] = &[
            ("alert", r#"{"message":"Claude needs your permission to use Bash","session_id":"abc-123"}"#),
            (
                "alert event",
                r#"{"hook_event_name":"Notification","message":"Ünïcode “quotes” \u0001 \u007f","session_id":"s"}"#,
            ),
            (
                "alert event",
                r#"{"hook_event_name":"PreToolUse","tool_name":"AskUserQuestion","tool_input":{"questions":[{"question":"Which one?"}]},"session_id":"q1"}"#,
            ),
            ("alert", r#"{"message":null,"tool_input":"str","session_id":7}"#),
            ("alert", r#"{"message":false}"#),
            ("alert", r#"{"tool_input":{"questions":[]},"session_id":"e"}"#),
            ("alert", r#"{"tool_input":{"questions":[{"question":7}]},"session_id":"n"}"#),
            ("alert", r#"{"message":"m wins","tool_input":"str"}"#),
            ("alert", r#"[1,2]"#),
            ("alert event", "null"),
            ("clear", "null"),
            ("clear", r#"{"session_id":1.5}"#),
            ("clear", r#"{"session_id":"abc-123"}"#),
            ("clear event", r#"{"hook_event_name":"PostToolUse","session_id":"s.2_x"}"#),
            ("clear", r#"{"session_id":""}"#),
            ("clear", r#"{"session_id":99}"#),
            ("event", r#"{"hook_event_name":"Stop","session_id":"we!rd $id/../x"}"#),
            ("event", r#"{"session_id":""}"#),
            ("event", r#"{"hook_event_name":{"a":[1,"b c"]},"session_id":true}"#),
            ("event", r#"{}"#),
            ("event", "not json at all"),
            ("event", ""),
            ("bogus event", r#"{"hook_event_name":"Stop","session_id":"z"}"#),
            ("event event", r#"{"hook_event_name":"Stop","session_id":"twice"}"#),
            ("alert", r#"{"message":"multi\nline\ttab","session_id":"m"}"#),
        ];
        for (mode, input) in cases {
            let a = dir("sh");
            let b = dir("rs");
            let home_a = a.join("home");
            fs::create_dir_all(&home_a).unwrap();
            let mut child = std::process::Command::new("sh")
                .arg(&script)
                .args(mode.split_whitespace())
                .env("HOME", &home_a)
                .stdin(std::process::Stdio::piped())
                .spawn()
                .unwrap();
            child.stdin.take().unwrap().write_all(input.as_bytes()).unwrap();
            child.wait().unwrap();
            // The script writes under its own hardcoded dir.
            let state_a = home_a.join(".config/claude-usage-monitor");
            let state_b = b.join("state");
            run(Mode::parse(&args(mode)), input, &state_b, 1_700_000_000);
            // The script predates the alert kind.
            let unkinded = |body: String| match body.strip_prefix("{\"k\":\"") {
                Some(rest) => format!("{{{}", rest.split_once("\",").map_or(rest, |(_, r)| r)),
                None => body,
            };
            let snapshot = |d: &Path| -> Vec<(String, String)> {
                let mut out: Vec<(String, String)> = fs::read_dir(d)
                    .map(|rd| {
                        rd.flatten()
                            .map(|e| e.file_name().to_string_lossy().into_owned())
                            // The script leaves attention.<pid> behind when jq fails.
                            .filter(|n| !n.starts_with("attention."))
                            .map(|n| {
                                let body = unkinded(fs::read_to_string(d.join(&n)).unwrap_or_default());
                                let body = body
                                    .lines()
                                    .map(|l| match l.split_once(' ') {
                                        Some((ts, rest))
                                            if n.starts_with("events.log") && ts.parse::<u64>().is_ok() =>
                                        {
                                            format!("TS {rest}\n")
                                        }
                                        _ => format!("{l}\n"),
                                    })
                                    .collect::<String>();
                                (n, body)
                            })
                            .collect()
                    })
                    .unwrap_or_default();
                out.sort();
                out
            };
            assert_eq!(snapshot(&state_a), snapshot(&state_b), "mode {mode:?} input {input}");
            let _ = fs::remove_dir_all(&a);
            let _ = fs::remove_dir_all(&b);
        }
    }
}
