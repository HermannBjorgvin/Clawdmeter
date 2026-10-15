//! Attention alerts from the hook (`clawdmeter hook alert|clear`).
//!
//! The hook writes `<state>/attention` (`{"m": message, "s": session id,
//! "k": kind}`, see [`AlertKind`])
//! when a session needs the user and touches `<state>/clear-<session id>` when
//! that session moves on. Each tick the daemon consumes both:
//!
//! * an alert file older than 60 s is dropped (written while the device was
//!   away), a bare `touch` still alerts with no message;
//! * a second alert from the session already alerting is ignored
//!   (AskUserQuestion notifies twice);
//! * a clear only ends the alert when it names the alerting session and is
//!   newer than the alert (nanosecond mtimes: both can land in one second).
//!
//! An alert also ends without a clear: when the device reports that it was
//! dismissed there (a tap, or its own timeout), and after [`ALERT_TTL`]
//! ([`TEST_ALERT_TTL`] for the Hooks page's test alert), because a session
//! that is closed or killed never sends its clear.

use std::collections::HashMap;
use std::fs;
use std::path::{Path, PathBuf};
use std::time::{Duration, SystemTime};

use unicode_normalization::UnicodeNormalization;

/// Alerts older than this when first seen are ignored.
pub const ATTENTION_MAX_AGE: Duration = Duration::from_secs(60);
/// An alert nothing has cleared ends this long after it was raised: the
/// device's own alert timeout, so both drop it together.
pub const ALERT_TTL: Duration = Duration::from_secs(2 * 60);
/// The test alert ends sooner: nothing ever clears its session.
pub const TEST_ALERT_TTL: Duration = Duration::from_secs(30);
/// The session id of the Hooks page's test alert.
pub const TEST_SESSION: &str = "clawdmeter-test";
/// Device fonts are ASCII and the status line holds this many characters.
pub const MESSAGE_LIMIT: usize = 120;

/// What a session is waiting for. The hook records it (`k`) from Claude
/// Code's hook input; which kinds alert is the `alert_on` setting.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum AlertKind {
    /// A tool needs permission (`Notification`, `permission_prompt`). Also
    /// a `Notification` with no type, as older Claude Code sends.
    Permission,
    /// Claude asks a question (`PreToolUse` on `AskUserQuestion`).
    Question,
    /// The prompt has sat idle, waiting for input (`idle_prompt`).
    Idle,
    /// An MCP server asks for input (`elicitation_dialog`).
    Elicitation,
    /// Any other notification type; follows [`AlertKind::Permission`].
    Other,
}

impl AlertKind {
    /// The `k` value the hook writes.
    pub fn as_str(self) -> &'static str {
        match self {
            AlertKind::Permission => "permission",
            AlertKind::Question => "question",
            AlertKind::Idle => "idle",
            AlertKind::Elicitation => "elicitation",
            AlertKind::Other => "other",
        }
    }

    /// Read `k`; a missing value (older hooks, the legacy script) is a
    /// permission request.
    pub fn parse(s: Option<&str>) -> Self {
        match s {
            None | Some("permission") => AlertKind::Permission,
            Some("question") => AlertKind::Question,
            Some("idle") => AlertKind::Idle,
            Some("elicitation") => AlertKind::Elicitation,
            Some(_) => AlertKind::Other,
        }
    }

    /// The kind for one hook input: `PreToolUse` is the question tool,
    /// `Notification` is told apart by its `notification_type`.
    pub fn from_hook(event: Option<&str>, notification_type: Option<&str>) -> Self {
        match (event, notification_type) {
            (Some("PreToolUse"), _) => AlertKind::Question,
            (_, None | Some("permission_prompt")) => AlertKind::Permission,
            (_, Some("idle_prompt")) => AlertKind::Idle,
            (_, Some("elicitation_dialog")) => AlertKind::Elicitation,
            (_, Some(_)) => AlertKind::Other,
        }
    }

    /// The fixed line the device shows for this kind, in place of Claude
    /// Code's own (often long) notification text.
    pub fn device_text(self) -> &'static str {
        match self {
            AlertKind::Question => "Question for you",
            AlertKind::Permission | AlertKind::Other => "Permission needed!",
            AlertKind::Idle => "Waiting for you!",
            AlertKind::Elicitation => "MCP request!",
        }
    }
}

/// One alert read from the attention file.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Alert {
    /// ASCII-folded message (may be empty).
    pub message: String,
    /// Claude Code session id (may be empty).
    pub session: String,
    /// What the session waits for.
    pub kind: AlertKind,
    /// The file's mtime.
    pub at: SystemTime,
}

impl Alert {
    /// What the device shows: the kind's [`AlertKind::device_text`], or the
    /// test alert's own text.
    pub fn device_message(&self) -> &str {
        if self.session == TEST_SESSION && !self.message.is_empty() {
            &self.message
        } else {
            self.kind.device_text()
        }
    }
}

/// Fold to ASCII (drop accents), collapse whitespace, cap at `limit` chars
/// with a `...` suffix.
pub fn to_ascii(text: &str, limit: usize) -> String {
    let folded: String = text.nfkd().filter(char::is_ascii).collect();
    let collapsed = folded.split_whitespace().collect::<Vec<_>>().join(" ");
    if collapsed.len() <= limit {
        collapsed
    } else {
        let mut cut = collapsed[..limit.saturating_sub(3)].trim_end().to_owned();
        cut.push_str("...");
        cut
    }
}

/// Reads and consumes the hook's files in one directory.
#[derive(Debug, Clone)]
pub struct AttentionInbox {
    dir: PathBuf,
}

impl AttentionInbox {
    /// Inbox over the daemon state dir.
    pub fn new(dir: impl Into<PathBuf>) -> Self {
        AttentionInbox { dir: dir.into() }
    }

    fn flag(&self) -> PathBuf {
        self.dir.join("attention")
    }

    /// Consume the attention file. `None` when absent or stale. The file is
    /// moved to a private name before it is read, so an alert the hook
    /// writes meanwhile lands in a new `attention` file for the next tick
    /// instead of being removed unread.
    pub fn take_alert(&self, now: SystemTime) -> Option<Alert> {
        let path = self.dir.join("attention.taking");
        fs::rename(self.flag(), &path).ok()?;
        let mtime = fs::metadata(&path).and_then(|m| m.modified()).ok();
        let raw = fs::read_to_string(&path).unwrap_or_default();
        let _ = fs::remove_file(&path);
        let mtime = mtime?;
        if now.duration_since(mtime).unwrap_or_default() >= ATTENTION_MAX_AGE {
            return None;
        }
        let v: serde_json::Value = serde_json::from_str(&raw).unwrap_or(serde_json::Value::Null);
        let field = |k: &str| match v.get(k) {
            Some(serde_json::Value::String(s)) => s.clone(),
            Some(serde_json::Value::Null) | None => String::new(),
            Some(other) => other.to_string(),
        };
        let kind = AlertKind::parse(v.get("k").and_then(|k| k.as_str()));
        Some(Alert { message: to_ascii(&field("m"), MESSAGE_LIMIT), session: field("s"), kind, at: mtime })
    }

    /// Consume every `clear-<sid>` file: `{sid: mtime}`.
    pub fn take_clears(&self) -> HashMap<String, SystemTime> {
        let mut out = HashMap::new();
        let Ok(rd) = fs::read_dir(&self.dir) else { return out };
        for entry in rd.flatten() {
            let name = entry.file_name();
            let Some(sid) = name.to_str().and_then(|n| n.strip_prefix("clear-")) else { continue };
            let mtime = entry.metadata().and_then(|m| m.modified()).unwrap_or(SystemTime::UNIX_EPOCH);
            if fs::remove_file(entry.path()).is_ok() {
                out.insert(sid.to_owned(), mtime);
            }
        }
        out
    }

    /// Drop leftovers (on connect).
    pub fn reset(&self) {
        let _ = fs::remove_file(self.flag());
        let _ = fs::remove_file(self.dir.join("attention.taking"));
        let _ = self.take_clears();
    }

    /// The directory this inbox reads.
    pub fn dir(&self) -> &Path {
        &self.dir
    }
}

/// What the next payload must say about attention.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct AttentionState {
    /// The alert on (or headed to) the screen, kept to match its clear.
    pub alert: Option<Alert>,
    /// The alert hasn't been sent yet.
    pub alert_unsent: bool,
    /// A clear for a shown alert hasn't been sent yet.
    pub clear_unsent: bool,
}

/// Fields to merge into the next payload.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct AttentionFields {
    /// `b`: beep (only when the user opted in).
    pub beep: bool,
    /// `m`: message for the status line.
    pub message: Option<String>,
    /// `x`: end the alert.
    pub clear: bool,
}

impl AttentionState {
    /// Fold one tick's files into the state.
    /// Returns whether a new alert went up (and is still up after the clears).
    pub fn ingest(&mut self, new_alert: Option<Alert>, clears: &HashMap<String, SystemTime>) -> bool {
        let mut raised = false;
        if let Some(a) = new_alert {
            let repeat = self.alert.as_ref().is_some_and(|cur| !cur.session.is_empty() && cur.session == a.session);
            if !repeat {
                self.alert = Some(a);
                self.alert_unsent = true;
                self.clear_unsent = false;
                raised = true;
            }
        }
        if let Some(cur) = &self.alert {
            if let Some(t) = clears.get(&cur.session) {
                if !cur.session.is_empty() && *t > cur.at {
                    // Never shown: nothing to clear on the device.
                    self.clear_unsent = !self.alert_unsent;
                    self.alert = None;
                    self.alert_unsent = false;
                    raised = false;
                }
            }
        }
        raised
    }

    /// End an alert that has been up for its time to live ([`ALERT_TTL`],
    /// [`TEST_ALERT_TTL`]). A shown one gets a clear so the device drops it
    /// too. Returns whether it ended.
    pub fn expire(&mut self, now: SystemTime) -> bool {
        let Some(cur) = &self.alert else { return false };
        let ttl = if cur.session == TEST_SESSION { TEST_ALERT_TTL } else { ALERT_TTL };
        if now.duration_since(cur.at).unwrap_or_default() < ttl {
            return false;
        }
        self.clear_unsent = !self.alert_unsent;
        self.alert = None;
        self.alert_unsent = false;
        true
    }

    /// The device ended the alert itself: forget it without sending a clear.
    pub fn dismissed(&mut self) {
        if !self.alert_unsent {
            self.alert = None;
            self.clear_unsent = false;
        }
    }

    /// Something attention-related is waiting to be sent.
    pub fn pending(&self) -> bool {
        self.alert_unsent || self.clear_unsent
    }

    /// An alert is currently up (sent or not).
    pub fn active(&self) -> bool {
        self.alert.is_some()
    }

    /// The fields for the next payload; `beep_on` is the `beep` setting.
    pub fn fields(&self, beep_on: bool) -> AttentionFields {
        let mut f = AttentionFields::default();
        if self.alert_unsent {
            f.beep = beep_on;
            f.message = self.alert.as_ref().map(|a| a.device_message().to_owned());
        }
        f.clear = self.clear_unsent;
        f
    }

    /// The payload carrying [`AttentionState::fields`] was written.
    pub fn mark_sent(&mut self) {
        self.alert_unsent = false;
        self.clear_unsent = false;
    }

    /// Forget everything (new connection).
    pub fn reset(&mut self) {
        *self = AttentionState::default();
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs::File;

    fn tmpdir(tag: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("cm-attn-{tag}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&d);
        fs::create_dir_all(&d).unwrap();
        d
    }

    fn set_mtime(p: &Path, t: SystemTime) {
        File::options().write(true).open(p).unwrap().set_modified(t).unwrap();
    }

    #[test]
    fn attention_flag() {
        let d = tmpdir("flag");
        let inbox = AttentionInbox::new(&d);
        let now = SystemTime::now();
        assert!(inbox.take_alert(now).is_none());

        File::create(d.join("attention")).unwrap();
        assert_eq!(inbox.take_alert(SystemTime::now()).unwrap().message, "");
        assert!(!d.join("attention").exists());

        fs::write(d.join("attention"), r#"{"m": "Você  aprova\na ação?", "s": "abc"}"#).unwrap();
        let a = inbox.take_alert(SystemTime::now()).unwrap();
        assert_eq!((a.message.as_str(), a.session.as_str()), ("Voce aprova a acao?", "abc"));
        assert_eq!(to_ascii(&"x".repeat(200), 120), "x".repeat(117) + "...");

        File::create(d.join("attention")).unwrap();
        set_mtime(&d.join("attention"), SystemTime::now() - ATTENTION_MAX_AGE - Duration::from_secs(1));
        assert!(inbox.take_alert(SystemTime::now()).is_none());
        assert!(!d.join("attention").exists());

        fs::write(d.join("attention"), "not json").unwrap();
        assert_eq!(inbox.take_alert(SystemTime::now()).unwrap().session, "");
        assert!(!d.join("attention.taking").exists(), "nothing left behind");
        let _ = fs::remove_dir_all(&d);
    }

    fn raised(session: &str, at: SystemTime) -> Alert {
        Alert { message: "Approve?".into(), session: session.into(), kind: AlertKind::Permission, at }
    }

    #[test]
    fn shown_alert_expires_with_a_clear() {
        let t0 = SystemTime::now();
        let mut s = AttentionState::default();
        s.ingest(Some(raised("abc", t0)), &HashMap::new());
        s.mark_sent();
        assert!(!s.expire(t0 + ALERT_TTL - Duration::from_secs(1)));
        assert!(s.active());
        assert!(s.expire(t0 + ALERT_TTL));
        assert!(!s.active());
        assert!(s.fields(false).clear, "the device drops it too");
        s.mark_sent();
        assert!(!s.pending());
    }

    #[test]
    fn unsent_alert_expires_quietly() {
        let t0 = SystemTime::now();
        let mut s = AttentionState::default();
        s.ingest(Some(raised("abc", t0)), &HashMap::new());
        assert!(s.expire(t0 + ALERT_TTL));
        assert!(!s.pending(), "never shown: nothing to send");
    }

    #[test]
    fn test_alert_expires_after_thirty_seconds() {
        let t0 = SystemTime::now();
        let mut s = AttentionState::default();
        s.ingest(Some(raised(TEST_SESSION, t0)), &HashMap::new());
        s.mark_sent();
        assert!(!s.expire(t0 + Duration::from_secs(29)));
        assert!(s.expire(t0 + TEST_ALERT_TTL));
        assert!(s.fields(false).clear);
    }

    #[test]
    fn device_dismissal_ends_a_shown_alert_without_a_clear() {
        let t0 = SystemTime::now();
        let mut s = AttentionState::default();
        s.ingest(Some(raised("abc", t0)), &HashMap::new());
        // Not on the device yet: a dismissal is about an older alert.
        s.dismissed();
        assert!(s.active() && s.alert_unsent);
        s.mark_sent();
        s.dismissed();
        assert!(!s.active());
        assert!(!s.pending(), "the device already dropped it");
        // The same session alerting again is a new alert.
        s.ingest(Some(raised("abc", t0 + Duration::from_secs(5))), &HashMap::new());
        assert!(s.active() && s.alert_unsent);
    }

    // Ported from test_attention.py::test_clears.
    #[test]
    fn clears() {
        let d = tmpdir("clears");
        let inbox = AttentionInbox::new(&d);
        File::create(d.join("clear-abc")).unwrap();
        File::create(d.join("unrelated")).unwrap();
        let c = inbox.take_clears();
        assert_eq!(c.keys().collect::<Vec<_>>(), vec!["abc"]);
        assert!(inbox.take_clears().is_empty());
        assert!(d.join("unrelated").exists());
        let _ = fs::remove_dir_all(&d);
    }

    fn alert(sid: &str, msg: &str, at: SystemTime) -> Alert {
        Alert { message: msg.into(), session: sid.into(), kind: AlertKind::Permission, at }
    }

    #[test]
    fn session_matching_like_bash() {
        let t0 = SystemTime::now();
        let none = HashMap::new();
        let mut st = AttentionState::default();

        st.ingest(Some(alert("abc", "Voce aprova a acao?", t0)), &none);
        assert!(st.alert_unsent);
        assert_eq!(
            st.fields(false),
            AttentionFields { beep: false, message: Some("Permission needed!".into()), clear: false }
        );
        assert!(st.fields(true).beep);
        st.mark_sent();

        // Repeat from the same session is ignored.
        st.ingest(Some(alert("abc", "Claude needs your permission", t0)), &none);
        assert!(!st.alert_unsent);
        assert_eq!(st.alert.as_ref().unwrap().message, "Voce aprova a acao?");

        // Clear for another session is ignored.
        let other: HashMap<_, _> = [("other".to_owned(), t0 + Duration::from_secs(2))].into();
        st.ingest(None, &other);
        assert!(st.alert.is_some() && !st.clear_unsent);

        // Clear for the alerting session, newer: hide.
        let same: HashMap<_, _> = [("abc".to_owned(), t0 + Duration::from_secs(2))].into();
        st.ingest(None, &same);
        assert!(st.alert.is_none() && st.clear_unsent);
        assert_eq!(st.fields(true), AttentionFields { beep: false, message: None, clear: true });
        st.mark_sent();

        // Same second, later by 0.5 s: still clears.
        let t1 = SystemTime::UNIX_EPOCH + Duration::from_nanos(1_760_000_000_100_000_000);
        st.ingest(Some(alert("q", "quick", t1)), &none);
        st.mark_sent();
        let q: HashMap<_, _> = [("q".to_owned(), t1 + Duration::from_millis(500))].into();
        st.ingest(None, &q);
        assert!(st.alert.is_none() && st.clear_unsent);

        // An older clear doesn't end a newer alert.
        st.mark_sent();
        st.ingest(Some(alert("z", "", t1)), &none);
        let old: HashMap<_, _> = [("z".to_owned(), t1 - Duration::from_secs(1))].into();
        st.ingest(None, &old);
        assert!(st.alert.is_some());
    }

    #[test]
    fn clear_before_send_sends_nothing() {
        let t0 = SystemTime::now();
        let mut st = AttentionState::default();
        st.ingest(Some(alert("s", "hi", t0)), &HashMap::new());
        let c: HashMap<_, _> = [("s".to_owned(), t0 + Duration::from_secs(1))].into();
        st.ingest(None, &c);
        assert!(!st.pending(), "never shown, nothing to clear");
    }

    #[test]
    fn device_shows_a_fixed_line_per_kind() {
        let at = SystemTime::now();
        let line = |kind, session: &str, message: &str| {
            let mut st = AttentionState::default();
            st.ingest(Some(Alert { message: message.into(), session: session.into(), kind, at }), &HashMap::new());
            st.fields(false).message.unwrap()
        };
        let hook_text = "Claude needs your permission to use Bash";
        assert_eq!(line(AlertKind::Question, "a", "Which file?"), "Question for you");
        assert_eq!(line(AlertKind::Permission, "a", hook_text), "Permission needed!");
        assert_eq!(line(AlertKind::Other, "a", hook_text), "Permission needed!");
        assert_eq!(line(AlertKind::Idle, "a", "Claude is waiting for your input"), "Waiting for you!");
        assert_eq!(line(AlertKind::Elicitation, "a", hook_text), "MCP request!");
        assert_eq!(line(AlertKind::Permission, "a", ""), "Permission needed!", "a bare touch still says why");
        assert_eq!(line(AlertKind::Permission, TEST_SESSION, "Clawdmeter test alert"), "Clawdmeter test alert");
    }

    #[test]
    fn alert_kinds_from_hook_input() {
        let k = AlertKind::from_hook;
        assert_eq!(k(Some("PreToolUse"), None), AlertKind::Question);
        assert_eq!(k(Some("Notification"), None), AlertKind::Permission);
        assert_eq!(k(Some("Notification"), Some("permission_prompt")), AlertKind::Permission);
        assert_eq!(k(Some("Notification"), Some("idle_prompt")), AlertKind::Idle);
        assert_eq!(k(Some("Notification"), Some("elicitation_dialog")), AlertKind::Elicitation);
        assert_eq!(k(Some("Notification"), Some("auth_success")), AlertKind::Other);
        for kind in [AlertKind::Permission, AlertKind::Question, AlertKind::Idle, AlertKind::Elicitation] {
            assert_eq!(AlertKind::parse(Some(kind.as_str())), kind);
        }
        assert_eq!(AlertKind::parse(None), AlertKind::Permission, "legacy files have no kind");
    }
}
