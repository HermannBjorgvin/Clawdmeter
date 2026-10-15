//! The fast clock: Claude Code hook events.
//!
//! The hook (event mode) appends `<unix_ts> <HookEventName> <session_id>` to
//! `<state>/events.log` and exits. [`EventTail`] follows that file; the
//! [`Activity`] tracker turns the lines into the payload's `a`, `e`/`es` and
//! `n` fields and tells the poll scheduler what the user is doing.

use std::collections::{HashMap, VecDeque};
use std::fs::{self, File};
use std::io::{self, Read, Seek, SeekFrom};
use std::path::{Path, PathBuf};

/// A hook event name.
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub enum HookKind {
    /// `UserPromptSubmit`: a prompt was sent.
    UserPromptSubmit,
    /// `PostToolUse`: a tool finished (also the end of a permission wait).
    PostToolUse,
    /// `Stop`: the turn is done.
    Stop,
    /// `Notification`: Claude needs the user.
    Notification,
    /// `PreToolUse` (the hook only fires it for AskUserQuestion).
    PreToolUse,
    /// Anything else, kept for forward compatibility.
    Other(String),
}

impl HookKind {
    /// Parse a hook event name.
    pub fn parse(s: &str) -> Self {
        match s {
            "UserPromptSubmit" => HookKind::UserPromptSubmit,
            "PostToolUse" => HookKind::PostToolUse,
            "Stop" => HookKind::Stop,
            "Notification" => HookKind::Notification,
            "PreToolUse" => HookKind::PreToolUse,
            other => HookKind::Other(other.to_owned()),
        }
    }
    /// The event means the session talked to the API (or is about to): a
    /// prompt, a tool call, the end of a turn. That starts a 5-hour window
    /// when none is running.
    pub fn sends_messages(&self) -> bool {
        matches!(self, HookKind::UserPromptSubmit | HookKind::PostToolUse | HookKind::PreToolUse | HookKind::Stop)
    }
}

/// One events.log line.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct HookEvent {
    /// Unix seconds.
    pub ts: i64,
    /// Event name.
    pub kind: HookKind,
    /// Session id (may be empty).
    pub session: String,
    /// The Claude Code config dir (account) the session runs under, when the
    /// hook could tell.
    pub account: Option<PathBuf>,
}

/// One whitespace-separated token and what follows it.
fn token(s: &str) -> Option<(&str, &str)> {
    let s = s.trim_start();
    let end = s.find(char::is_whitespace).unwrap_or(s.len());
    (end > 0).then(|| (&s[..end], &s[end..]))
}

/// Parse `<unix_ts> <HookEventName> [<session_id> [<config dir>]]`; the
/// config dir is the rest of the line (it may hold spaces). Fractional
/// timestamps are accepted (truncated). Returns `None` for anything else.
pub fn parse_line(line: &str) -> Option<HookEvent> {
    let (ts_raw, rest) = token(line)?;
    let ts = match ts_raw.split_once('.') {
        Some((s, frac)) if frac.bytes().all(|b| b.is_ascii_digit()) => s.parse().ok()?,
        Some(_) => return None,
        None => ts_raw.parse().ok()?,
    };
    let (kind, rest) = token(rest)?;
    let kind = HookKind::parse(kind);
    let (session, rest) = token(rest).unwrap_or(("", ""));
    // The hook writes "-" when a payload has no session id.
    let session = if session == "-" { "" } else { session }.to_owned();
    let account = Some(rest.trim()).filter(|a| !a.is_empty()).map(PathBuf::from);
    Some(HookEvent { ts, kind, session, account })
}

/// How much history to read at start-up for seeding the tracker.
pub const SEED_BYTES: u64 = 64 * 1024;

/// Follows events.log by byte offset. The hook rolls the log to
/// `events.log.1` past ~256 KB (`mv -f`); when that happens the unread tail of
/// the old file is drained from `.1` before following the new one.
#[derive(Debug)]
pub struct EventTail {
    path: PathBuf,
    offset: u64,
    partial: String,
    ident: FileIdent,
    /// Tell files apart by inode too (Unix). Windows has no stable file id
    /// in std, so there the first line alone tells a rolled log apart.
    inodes: bool,
}

/// What tells one log file from the next: its inode where there is one, and
/// its first line (a new log starts with a newer timestamp).
#[derive(Debug, Clone, Default, PartialEq, Eq)]
struct FileIdent {
    ino: Option<u64>,
    head: Option<String>,
}

/// The first line can't be longer than this (a timestamp, an event name and
/// a session id); a longer prefix without a newline is still being written.
const HEAD_BYTES: u64 = 256;

impl FileIdent {
    fn of(path: &Path, meta: &fs::Metadata, inodes: bool) -> Self {
        let head = read_range(path, 0, meta.len().min(HEAD_BYTES))
            .ok()
            .and_then(|s| s.split_once('\n').map(|(line, _)| line.to_owned()));
        FileIdent { ino: if inodes { inode(meta) } else { None }, head }
    }

    /// Known to be a different file from `prev`: something both know differs.
    fn differs(&self, prev: &FileIdent) -> bool {
        fn known_and_differ<T: PartialEq>(a: &Option<T>, b: &Option<T>) -> bool {
            matches!((a, b), (Some(a), Some(b)) if a != b)
        }
        known_and_differ(&self.ino, &prev.ino) || known_and_differ(&self.head, &prev.head)
    }
}

#[cfg(unix)]
fn inode(m: &fs::Metadata) -> Option<u64> {
    use std::os::unix::fs::MetadataExt;
    Some(m.ino())
}

#[cfg(not(unix))]
fn inode(_m: &fs::Metadata) -> Option<u64> {
    None
}

impl EventTail {
    /// Start at the end of the file. Returns the tail plus the last
    /// [`SEED_BYTES`] of history for [`Activity::seed`].
    pub fn open(path: impl Into<PathBuf>) -> (Self, Vec<HookEvent>) {
        let path = path.into();
        let meta = fs::metadata(&path).ok();
        let len = meta.as_ref().map_or(0, |m| m.len());
        let seed = read_range(&path, len.saturating_sub(SEED_BYTES), len)
            .map(|s| {
                // Drop a partial first line when we started mid-file.
                let s = if len > SEED_BYTES { s.split_once('\n').map_or("", |x| x.1).to_owned() } else { s };
                s.lines().filter_map(parse_line).collect()
            })
            .unwrap_or_default();
        let inodes = cfg!(unix);
        let ident = meta.as_ref().map(|m| FileIdent::of(&path, m, inodes)).unwrap_or_default();
        (EventTail { path, offset: len, partial: String::new(), ident, inodes }, seed)
    }

    /// True when the log (or its rolled copy) exists: the hooks are installed.
    pub fn exists(&self) -> bool {
        self.path.exists() || self.rolled().exists()
    }

    fn rolled(&self) -> PathBuf {
        let mut p = self.path.clone().into_os_string();
        p.push(".1");
        PathBuf::from(p)
    }

    /// New complete lines since the last call.
    pub fn poll(&mut self) -> Vec<HookEvent> {
        let mut out = Vec::new();
        let Ok(meta) = fs::metadata(&self.path) else {
            // Rolled and not recreated yet: finish the old file.
            if self.offset > 0 {
                self.drain_rolled(&mut out);
            }
            return out;
        };
        let len = meta.len();
        let ident = FileIdent::of(&self.path, &meta, self.inodes);
        // A different file means the hook rolled the log: finish the old one
        // from `.1`. Shrinking in place is a truncation: just start over.
        if ident.differs(&self.ident) {
            if self.offset > 0 {
                self.drain_rolled(&mut out);
            }
        } else if len < self.offset {
            self.offset = 0;
            self.partial.clear();
        }
        self.ident = ident;
        if len > self.offset {
            if let Ok(chunk) = read_range(&self.path, self.offset, len) {
                self.offset = len;
                self.consume(&chunk, &mut out);
            }
        }
        out
    }

    /// Read what's left of the rolled file past our offset, if it is the file
    /// we were following (same identity, at least as long as what we read).
    fn drain_rolled(&mut self, out: &mut Vec<HookEvent>) {
        let rolled = self.rolled();
        if let Ok(meta) = fs::metadata(&rolled) {
            let len = meta.len();
            if len > self.offset && !FileIdent::of(&rolled, &meta, self.inodes).differs(&self.ident) {
                if let Ok(chunk) = read_range(&rolled, self.offset, len) {
                    self.consume(&chunk, out);
                }
            }
        }
        self.offset = 0;
        self.partial.clear();
    }

    fn consume(&mut self, chunk: &str, out: &mut Vec<HookEvent>) {
        self.partial.push_str(chunk);
        while let Some(i) = self.partial.find('\n') {
            let line: String = self.partial.drain(..=i).collect();
            if let Some(ev) = parse_line(line.trim_end()) {
                out.push(ev);
            }
        }
    }

    /// The followed path.
    pub fn path(&self) -> &Path {
        &self.path
    }
}

fn read_range(path: &Path, from: u64, to: u64) -> io::Result<String> {
    let mut f = File::open(path)?;
    f.seek(SeekFrom::Start(from))?;
    let mut buf = Vec::with_capacity((to - from) as usize);
    f.take(to - from).read_to_end(&mut buf)?;
    Ok(String::from_utf8_lossy(&buf).into_owned())
}

/// Latest-event codes for the payload's `e` field.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum EventCode {
    /// `p`: a prompt was sent.
    Prompt,
    /// `s`: the turn stopped.
    Stop,
    /// `b`: a prompt after 10+ quiet minutes ("you're back").
    Back,
}

impl EventCode {
    /// The one-letter payload value.
    pub fn as_str(self) -> &'static str {
        match self {
            EventCode::Prompt => "p",
            EventCode::Stop => "s",
            EventCode::Back => "b",
        }
    }
}

/// Quiet gap after which a prompt counts as "back".
pub const BACK_AFTER_SECS: i64 = 600;
/// Window for counting a session as active (`n`).
pub const SESSION_ACTIVE_SECS: i64 = 300;
/// Window for the PostToolUse rate (`a`).
pub const ACTIVITY_WINDOW_SECS: i64 = 60;

/// Tool calls in the last minute → activity level 0-3.
pub fn activity_level(tool_calls_last_minute: usize) -> u8 {
    match tool_calls_last_minute {
        0 => 0,
        1..=3 => 1,
        4..=9 => 2,
        _ => 3,
    }
}

/// State derived from hook events.
#[derive(Debug, Clone, Default)]
pub struct Activity {
    tool_ts: VecDeque<i64>,
    sessions: HashMap<String, i64>,
    /// Timestamp of the newest event of any kind.
    pub last_event_ts: Option<i64>,
    /// Newest `UserPromptSubmit`.
    pub last_prompt_ts: Option<i64>,
    /// Newest `Stop`.
    pub last_stop_ts: Option<i64>,
    /// Newest `PostToolUse`.
    pub last_tool_ts: Option<i64>,
    /// Latest `e` and its sequence number `es` (0 = none yet).
    pub latest: Option<EventCode>,
    /// Sequence number, bumped on every live p/s/b.
    pub seq: u32,
    /// Any event has ever been seen (the hooks are installed).
    pub hooks_seen: bool,
}

/// What a live event means for the daemon.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Signal {
    /// A prompt was sent (poll now, then on the working cadence).
    Prompt {
        /// After 10+ quiet minutes.
        back: bool,
    },
    /// The turn stopped (poll once ~10 s later).
    Stop,
    /// A tool ran.
    Tool,
    /// Something else (Notification, PreToolUse...).
    Other,
}

impl Activity {
    /// Load history without emitting `e` (old events must not replay on the
    /// device after a daemon restart).
    pub fn seed(&mut self, events: &[HookEvent]) {
        for ev in events {
            self.record(ev);
        }
    }

    fn record(&mut self, ev: &HookEvent) {
        self.hooks_seen = true;
        self.last_event_ts = Some(self.last_event_ts.map_or(ev.ts, |t| t.max(ev.ts)));
        if !ev.session.is_empty() {
            let e = self.sessions.entry(ev.session.clone()).or_insert(ev.ts);
            *e = (*e).max(ev.ts);
        }
        match ev.kind {
            HookKind::PostToolUse => {
                self.tool_ts.push_back(ev.ts);
                self.last_tool_ts = Some(ev.ts);
            }
            HookKind::UserPromptSubmit => self.last_prompt_ts = Some(ev.ts),
            HookKind::Stop => self.last_stop_ts = Some(ev.ts),
            _ => {}
        }
    }

    /// Feed one live event; returns its meaning.
    pub fn ingest(&mut self, ev: &HookEvent) -> Signal {
        let quiet_before = self.last_event_ts.map(|t| ev.ts - t);
        self.record(ev);
        match ev.kind {
            HookKind::UserPromptSubmit => {
                let back = quiet_before.is_some_and(|gap| gap >= BACK_AFTER_SECS);
                self.bump(if back { EventCode::Back } else { EventCode::Prompt });
                Signal::Prompt { back }
            }
            HookKind::Stop => {
                self.bump(EventCode::Stop);
                Signal::Stop
            }
            HookKind::PostToolUse => Signal::Tool,
            _ => Signal::Other,
        }
    }

    fn bump(&mut self, code: EventCode) {
        self.latest = Some(code);
        self.seq = self.seq.wrapping_add(1).max(1);
    }

    /// Drop history that no longer matters.
    pub fn prune(&mut self, now: i64) {
        while self.tool_ts.front().is_some_and(|t| now - t >= ACTIVITY_WINDOW_SECS) {
            self.tool_ts.pop_front();
        }
        self.sessions.retain(|_, t| now - *t < SESSION_ACTIVE_SECS);
    }

    /// `a`: activity level 0-3 from PostToolUse in the last minute.
    pub fn level(&self, now: i64) -> u8 {
        activity_level(self.tool_ts.iter().filter(|t| now - **t < ACTIVITY_WINDOW_SECS && **t <= now).count())
    }

    /// `n`: sessions with any hook event in the last 5 minutes.
    pub fn sessions_active(&self, now: i64) -> u32 {
        self.sessions.values().filter(|t| now - **t < SESSION_ACTIVE_SECS).count() as u32
    }

    /// A prompt is in flight (no Stop after it) and something happened in the
    /// last 10 minutes, or a tool ran in the last minute after the last Stop.
    pub fn working(&self, now: i64) -> bool {
        let prompt_open = match (self.last_prompt_ts, self.last_stop_ts) {
            (Some(p), Some(s)) => p > s,
            (Some(_), None) => true,
            _ => false,
        };
        let recent = self.last_event_ts.is_some_and(|t| now - t < BACK_AFTER_SECS);
        // A tool after the last Stop (e.g. another session, or a permission
        // approval) also counts.
        let tool_open = self
            .last_tool_ts
            .is_some_and(|t| now - t < ACTIVITY_WINDOW_SECS && self.last_stop_ts.is_none_or(|s| t > s));
        (prompt_open && recent) || tool_open
    }

    /// Seconds since the newest event, `None` if none ever.
    pub fn quiet_for(&self, now: i64) -> Option<i64> {
        self.last_event_ts.map(|t| (now - t).max(0))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;

    fn ev(ts: i64, kind: &str, sid: &str) -> HookEvent {
        HookEvent { ts, kind: HookKind::parse(kind), session: sid.into(), account: None }
    }

    #[test]
    fn parse_lines() {
        assert_eq!(parse_line("1760000000 Stop abc"), Some(ev(1_760_000_000, "Stop", "abc")));
        assert_eq!(parse_line("1760000000.25 PostToolUse s1"), Some(ev(1_760_000_000, "PostToolUse", "s1")));
        assert_eq!(parse_line("1760000000 UserPromptSubmit"), Some(ev(1_760_000_000, "UserPromptSubmit", "")));
        assert_eq!(parse_line("1760000000 SessionStart x").unwrap().kind, HookKind::Other("SessionStart".into()));
        assert_eq!(parse_line(""), None);
        assert_eq!(parse_line("abc Stop x"), None);
        assert_eq!(parse_line("1760000000"), None);
        assert_eq!(parse_line("12.x Stop"), None);
        // The account is the rest of the line, spaces and all.
        let e = parse_line("1760000000 Stop abc /home/me/My Claude").unwrap();
        assert_eq!(e.account.as_deref(), Some(std::path::Path::new("/home/me/My Claude")));
        assert_eq!(e.session, "abc");
        assert_eq!(parse_line("1760000000 Stop - /home/me/.claude").unwrap().session, "");
        assert_eq!(parse_line("1760000000 Stop abc  ").unwrap().account, None);
    }

    #[test]
    fn levels() {
        assert_eq!(activity_level(0), 0);
        assert_eq!(activity_level(1), 1);
        assert_eq!(activity_level(3), 1);
        assert_eq!(activity_level(4), 2);
        assert_eq!(activity_level(9), 2);
        assert_eq!(activity_level(10), 3);
    }

    #[test]
    fn activity_fields() {
        let mut a = Activity::default();
        let t = 1_000_000;
        assert_eq!(a.ingest(&ev(t, "UserPromptSubmit", "s1")), Signal::Prompt { back: false });
        assert_eq!((a.latest, a.seq), (Some(EventCode::Prompt), 1));
        for i in 0..5 {
            a.ingest(&ev(t + i, "PostToolUse", "s1"));
        }
        a.ingest(&ev(t + 5, "PostToolUse", "s2"));
        assert_eq!(a.level(t + 10), 2);
        assert_eq!(a.sessions_active(t + 10), 2);
        assert!(a.working(t + 10));
        assert_eq!(a.level(t + 70), 0);
        assert_eq!(a.ingest(&ev(t + 80, "Stop", "s1")), Signal::Stop);
        assert_eq!((a.latest, a.seq), (Some(EventCode::Stop), 2));
        assert!(!a.working(t + 90));
        assert_eq!(a.sessions_active(t + 80 + 299), 1);
        assert_eq!(a.sessions_active(t + 80 + 300), 0);
        // 10+ quiet minutes then a prompt: back.
        assert_eq!(a.ingest(&ev(t + 80 + 600, "UserPromptSubmit", "s1")), Signal::Prompt { back: true });
        assert_eq!(a.latest, Some(EventCode::Back));
        a.prune(t + 2000);
        assert_eq!(a.sessions_active(t + 2000), 0);
    }

    #[test]
    fn seed_does_not_emit() {
        let mut a = Activity::default();
        a.seed(&[ev(10, "UserPromptSubmit", "s"), ev(20, "Stop", "s")]);
        assert_eq!((a.latest, a.seq), (None, 0));
        assert!(a.hooks_seen);
        assert_eq!(a.last_stop_ts, Some(20));
        // First live prompt after a long seeded quiet is "back".
        assert_eq!(a.ingest(&ev(20 + 601, "UserPromptSubmit", "s")), Signal::Prompt { back: true });
    }

    #[test]
    fn tail_follows_truncates_and_rotates() {
        follows_truncates_and_rotates("inode", true);
    }

    // Windows: std has no file id, so the first line tells the logs apart.
    #[test]
    fn tail_rotates_without_inodes() {
        follows_truncates_and_rotates("head", false);
    }

    #[test]
    fn stale_rolled_file_is_not_replayed() {
        let dir = std::env::temp_dir().join(format!("cm-tail-stale-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        let path = dir.join("events.log");
        fs::write(dir.join("events.log.1"), "1 Stop old\n2 Stop old\n3 Stop old\n").unwrap();
        fs::write(&path, "10 Stop a\n").unwrap();
        let (mut tail, _) = EventTail::open(&path);
        tail.inodes = false;
        tail.ident.ino = None;
        // Rewritten in place with a new first line: not the old roll's tail.
        fs::write(&path, "20 Stop b\n").unwrap();
        assert_eq!(tail.poll(), vec![ev(20, "Stop", "b")]);
        let _ = fs::remove_dir_all(&dir);
    }

    fn follows_truncates_and_rotates(tag: &str, inodes: bool) {
        let dir = std::env::temp_dir().join(format!("cm-tail-{tag}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        let path = dir.join("events.log");
        let rolled = dir.join("events.log.1");
        fs::write(&path, "1 UserPromptSubmit old\n2 Stop -\n").unwrap();
        let (mut tail, seed) = EventTail::open(&path);
        if !inodes {
            tail.inodes = false;
            tail.ident.ino = None;
        }
        assert_eq!(seed.len(), 2);
        assert_eq!(seed[1].session, "", "\"-\" means no session");
        assert!(tail.poll().is_empty(), "history isn't replayed");

        let mut f = fs::OpenOptions::new().append(true).open(&path).unwrap();
        write!(f, "3 PostToolUse a\n4 Post").unwrap();
        assert_eq!(tail.poll(), vec![ev(3, "PostToolUse", "a")]);
        writeln!(f, "ToolUse a").unwrap();
        assert_eq!(tail.poll(), vec![ev(4, "PostToolUse", "a")]);

        // Unread line, then the hook rolls the log (mv -f) and appends anew.
        writeln!(f, "5 Stop a").unwrap();
        drop(f);
        fs::rename(&path, &rolled).unwrap();
        assert_eq!(tail.poll(), vec![ev(5, "Stop", "a")], "drained from .1 while the new file is absent");
        fs::write(&path, "6 Stop b\n").unwrap();
        assert_eq!(tail.poll(), vec![ev(6, "Stop", "b")]);

        // Rolled again with an unread line, new file already present.
        let mut f = fs::OpenOptions::new().append(true).open(&path).unwrap();
        writeln!(f, "7 Stop c").unwrap();
        drop(f);
        fs::rename(&path, &rolled).unwrap();
        fs::write(&path, "8 Stop d\n").unwrap();
        assert_eq!(tail.poll(), vec![ev(7, "Stop", "c"), ev(8, "Stop", "d")]);
        assert!(tail.exists());

        // Truncated in place.
        fs::write(&path, "").unwrap();
        assert!(tail.poll().is_empty());
        fs::write(&path, "9 Stop e\n").unwrap();
        assert_eq!(tail.poll(), vec![ev(9, "Stop", "e")]);
        let _ = fs::remove_dir_all(&dir);
    }
}
