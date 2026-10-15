//! Usage from `claude --output-format json -p /usage`.
//!
//! The CLI answers locally (`"local_command": "usage"`, zero tokens), so
//! polling it never sends a message or starts a window. The numbers live in
//! human-readable lines of the `result` text. A subscription account
//! (Pro / Max / Team / seat-based Enterprise) prints:
//!
//! ```text
//! You are currently using your subscription to power your Claude Code usage
//!
//! Current session: 15% used · resets Oct 13, 7am (Atlantic/Reykjavik)
//! Current week (all models): 6% used · resets Oct 18, 10pm (Atlantic/Reykjavik)
//! Current week (Fable): 0% used · resets Oct 18, 10pm (Atlantic/Reykjavik)
//! ```
//!
//! The first line says "using your overages" instead while extra usage is
//! paying for requests. An account billed against a spend limit (the old
//! daemons' "enterprise" header path) prints one of:
//!
//! ```text
//! Spend limit: $41.20 / $200.00 spent this month (21%) · resets Nov 1, 12am (UTC)
//! Spend limit: 21% used · resets Nov 1, 12am (UTC)
//! ```
//!
//! Reset times are local wall-clock text with an IANA zone; the year is only
//! printed when it differs from the current one, otherwise the parser takes
//! the next occurrence. A missing session line (or one with no reset) means
//! the 5-hour window hasn't started. A text with no session, week or spend
//! line is a parse failure, never a zero.

use std::io::Read;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::time::{Duration, Instant};

use chrono::{DateTime, Datelike, Duration as ChronoDuration, NaiveDate, NaiveTime, TimeZone, Utc};
use chrono_tz::Tz;

use crate::backoff::Backoff;

/// One usage window (session or week).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Window {
    /// Percent used, rounded. Can exceed 100 on overage.
    pub pct: u32,
    /// When the window resets; `None` when the line has no (parseable) reset.
    pub resets_at: Option<DateTime<Utc>>,
}

/// The period a spend limit covers (`spent today / this week / this month`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SpendPeriod {
    /// `today`.
    Daily,
    /// `this week`.
    Weekly,
    /// `this month` (also assumed when the line names no period: the
    /// enterprise spend-limit period is monthly today).
    Monthly,
}

/// A `Spend limit:` line: an account billed against a spending cap.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Spend {
    /// Percent of the limit spent, rounded. Can exceed 100.
    pub pct: u32,
    /// When the period resets.
    pub resets_at: Option<DateTime<Utc>>,
    /// The period the limit covers, when the line names one.
    pub period: Option<SpendPeriod>,
}

/// Everything `/usage` told us for one account.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct UsageReading {
    /// `Current session` (5-hour window). `None` = window not started.
    pub session: Option<Window>,
    /// `Current week (all models)`.
    pub week: Option<Window>,
    /// `Current week (<model>)` lines, in order. Not sent to the device yet.
    pub models: Vec<(String, Window)>,
    /// `Spend limit:` (spend-capped accounts, sent as the enterprise view).
    pub spend: Option<Spend>,
    /// The CLI says extra usage ("overages") is paying for requests.
    pub overage: bool,
}

impl UsageReading {
    /// The device's main meter (`s`): session % used (0 when the window
    /// hasn't started), or the spend % for a spend-capped account.
    pub fn session_pct(&self) -> u32 {
        match (&self.session, &self.spend) {
            (Some(w), _) => w.pct,
            (None, Some(sp)) if self.week.is_none() => sp.pct,
            _ => 0,
        }
    }

    /// True when a 5-hour window is running (a session line with a reset).
    pub fn window_started(&self) -> bool {
        self.session.as_ref().is_some_and(|w| w.resets_at.is_some())
    }

    /// A spend-capped account with no session/week windows: the device shows
    /// it in its enterprise layout (spend % and billing period).
    pub fn is_spend_only(&self) -> bool {
        self.session.is_none() && self.week.is_none() && self.spend.is_some()
    }

    /// A window (or the spend cap) is used up and extra usage isn't covering
    /// it.
    pub fn limited(&self) -> bool {
        if self.overage {
            return false;
        }
        let full = |w: &Option<Window>| w.as_ref().is_some_and(|w| w.pct >= 100);
        full(&self.session) || full(&self.week) || self.spend.as_ref().is_some_and(|s| s.pct >= 100)
    }
}

/// Why a poll produced no reading.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum UsageError {
    /// `claude` could not be found or started.
    Spawn(String),
    /// The CLI ran longer than the timeout and was killed.
    Timeout,
    /// The CLI exited non-zero or printed something that isn't its JSON.
    BadOutput(String),
    /// The CLI says this account needs `/login`.
    NotLoggedIn(String),
    /// The account isn't on a subscription plan (API key billing).
    NotSubscription,
    /// The text had neither a session nor a week line. In practice this is
    /// what a rate-limited usage endpoint looks like (see [`crate::backoff`]).
    NoUsageLines(String),
    /// No usage lines, and Claude Code's debug log says its usage endpoint
    /// answered 429 and it won't ask again for this long (s).
    RateLimited {
        /// Seconds until Claude Code asks the endpoint again.
        retry_after_secs: i64,
    },
    /// The usage endpoint answered 429 ([`crate::oauth`]).
    Throttled {
        /// Its `retry-after`, when it sent one: honoured exactly.
        retry_after_secs: Option<i64>,
    },
    /// The account's OAuth token has expired; Claude Code refreshes it, we
    /// don't.
    TokenExpired,
    /// The usage endpoint refused the token (401 / 403).
    TokenRejected,
    /// The usage endpoint couldn't be reached.
    Offline(String),
}

impl std::fmt::Display for UsageError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            UsageError::Spawn(e) => write!(f, "could not run claude: {e}"),
            UsageError::Timeout => write!(f, "claude /usage timed out"),
            UsageError::BadOutput(e) => write!(f, "unexpected claude output: {e}"),
            UsageError::NotLoggedIn(e) => write!(f, "not logged in: {e}"),
            UsageError::NotSubscription => write!(f, "account is not on a subscription plan"),
            UsageError::NoUsageLines(e) => write!(f, "no usage lines in /usage output: {e}"),
            UsageError::RateLimited { retry_after_secs } => {
                write!(f, "usage endpoint rate-limited (429); claude retries in {retry_after_secs}s")
            }
            UsageError::Throttled { retry_after_secs: Some(s) } => write!(f, "usage endpoint 429, retry-after {s}s"),
            UsageError::Throttled { retry_after_secs: None } => write!(f, "usage endpoint 429"),
            UsageError::TokenExpired => write!(f, "OAuth token expired; waiting for Claude Code to refresh it"),
            UsageError::TokenRejected => write!(f, "usage endpoint refused the OAuth token"),
            UsageError::Offline(e) => write!(f, "usage endpoint unreachable: {e}"),
        }
    }
}

impl std::error::Error for UsageError {}

impl UsageError {
    /// Errors that say this account can't be read at all until the user acts,
    /// as opposed to a glitch worth retrying.
    pub fn is_auth(&self) -> bool {
        matches!(self, UsageError::NotLoggedIn(_) | UsageError::NotSubscription)
    }

    /// A miss that may have reached Claude Code's usage endpoint and is
    /// worth retrying later: the account backs off ([`crate::backoff`]).
    /// Not auth problems (the user has to act) and not a missing CLI (it
    /// never got as far as the endpoint).
    pub fn is_transient(&self) -> bool {
        matches!(
            self,
            UsageError::Timeout
                | UsageError::BadOutput(_)
                | UsageError::NoUsageLines(_)
                | UsageError::RateLimited { .. }
                | UsageError::Throttled { .. }
                | UsageError::Offline(_)
        )
    }

    /// A 429 from the usage endpoint (ours, or Claude Code's).
    pub fn is_throttle(&self) -> bool {
        matches!(self, UsageError::Throttled { .. } | UsageError::RateLimited { .. })
    }

    /// Claude Code's 429 memory, when its debug log told us.
    pub fn retry_after(&self) -> Option<i64> {
        match self {
            UsageError::RateLimited { retry_after_secs } => Some(*retry_after_secs),
            _ => None,
        }
    }

    /// One or two short sentences for the app's UI; `Display` keeps the
    /// details for logs. `retry_in` is how long the account's backoff has
    /// left (s), when it is backing off.
    pub fn user_message(&self, retry_in: Option<i64>) -> String {
        let again = match retry_in.filter(|s| *s > 0) {
            Some(s) => {
                let m = (s + 59) / 60;
                format!("Trying again in about {m} minute{}.", if m == 1 { "" } else { "s" })
            }
            None => "Retrying.".to_owned(),
        };
        match self {
            UsageError::Spawn(_) => {
                "Couldn't run Claude Code. Check its installation and the claude_path setting.".into()
            }
            UsageError::Timeout => format!("Claude Code took too long to answer. {again}"),
            UsageError::BadOutput(_) => format!("Couldn't read Claude Code's usage response. {again}"),
            UsageError::NotLoggedIn(_) => "Sign in to Claude Code for this account folder.".into(),
            UsageError::NotSubscription => "This account doesn't report subscription usage limits.".into(),
            UsageError::NoUsageLines(_) => format!("No usage data was returned. {again}"),
            UsageError::RateLimited { .. } | UsageError::Throttled { .. } => {
                format!("Usage requests are temporarily limited. {again}")
            }
            UsageError::TokenExpired | UsageError::TokenRejected => {
                "Waiting for Claude Code to renew its sign-in. Using Claude Code or the Claude desktop app renews it."
                    .into()
            }
            UsageError::Offline(_) => format!("Couldn't reach Claude. {again}"),
        }
    }
}

/// "Usage paused by Anthropic until 14:05." for a block ending at `until`
/// (unix seconds), shown at `utc_offset` (seconds east of UTC).
pub fn paused_message(until: i64, utc_offset: i64) -> String {
    let t = DateTime::<Utc>::from_timestamp(until + utc_offset, 0).unwrap_or_default();
    format!("Usage paused by Anthropic until {}.", t.format("%H:%M"))
}

fn snippet(s: &str) -> String {
    let s = s.trim();
    let mut out: String = s.chars().take(160).collect();
    if s.chars().count() > 160 {
        out.push('…');
    }
    out
}

/// Parse the CLI's JSON (`--output-format json`) and then its `result` text.
pub fn parse_cli_output(stdout: &str, now: DateTime<Utc>) -> Result<UsageReading, UsageError> {
    // The CLI prints one JSON object; tolerate leading noise lines.
    let json_start = stdout.find('{').ok_or_else(|| UsageError::BadOutput(snippet(stdout)))?;
    let v: serde_json::Value = serde_json::from_str(stdout[json_start..].trim())
        .map_err(|e| UsageError::BadOutput(format!("{e}: {}", snippet(stdout))))?;
    let result = v.get("result").and_then(|r| r.as_str()).unwrap_or("");
    let is_error = v.get("is_error").and_then(|b| b.as_bool()).unwrap_or(false);
    // Lines first: a hint like "/login" elsewhere in a good answer must not
    // turn it into an error.
    match parse_usage_text(result, now) {
        Err(e) if is_error && !e.is_auth() => Err(UsageError::BadOutput(snippet(result))),
        other => other,
    }
}

pub(crate) fn classify_auth(text: &str) -> Option<UsageError> {
    let l = text.to_ascii_lowercase();
    if l.contains("not logged in")
        || l.contains("/login")
        || l.contains("invalid api key")
        || l.contains("oauth token has expired")
    {
        return Some(UsageError::NotLoggedIn(snippet(text)));
    }
    if l.contains("using your api key") || l.contains("only available for subscription") {
        return Some(UsageError::NotSubscription);
    }
    None
}

/// Parse the human-readable `/usage` text.
pub fn parse_usage_text(text: &str, now: DateTime<Utc>) -> Result<UsageReading, UsageError> {
    let mut r = UsageReading { overage: text.contains("using your overages"), ..Default::default() };
    let mut matched = false;
    for raw in text.lines() {
        let line = raw.trim();
        if let Some(body) = line.strip_prefix("Spend limit:") {
            match parse_spend(body, now) {
                Some(sp) => {
                    r.spend = Some(sp);
                    matched = true;
                }
                None => log::warn!("/usage line stopped matching: {line:?}"),
            }
            continue;
        }
        let Some(rest) = line.strip_prefix("Current ") else { continue };
        let Some((label, body)) = rest.split_once(':') else { continue };
        let Some(window) = parse_window(body, now) else {
            log::warn!("/usage line stopped matching: {line:?}");
            continue;
        };
        let label = label.trim();
        if label.eq_ignore_ascii_case("session") {
            r.session = Some(window);
            matched = true;
        } else if let Some(inner) = label
            .strip_prefix("week")
            .map(str::trim)
            .and_then(|s| s.strip_prefix('('))
            .and_then(|s| s.strip_suffix(')'))
        {
            if inner.eq_ignore_ascii_case("all models") {
                r.week = Some(window);
            } else {
                r.models.push((inner.to_owned(), window));
            }
            matched = true;
        } else if label.eq_ignore_ascii_case("week") {
            r.week = Some(window);
            matched = true;
        }
    }
    if matched {
        Ok(r)
    } else {
        Err(classify_auth(text).unwrap_or_else(|| UsageError::NoUsageLines(snippet(text))))
    }
}

/// `" $41.20 / $200.00 spent this month (21%) · resets Nov 1, 12am (UTC)"` or
/// `" 21% used · resets Nov 1, 12am (UTC)"` → Spend.
fn parse_spend(body: &str, now: DateTime<Utc>) -> Option<Spend> {
    let (head, reset) = match body.find("resets") {
        Some(i) => (&body[..i], Some(body[i + "resets".len()..].trim())),
        None => (body, None),
    };
    let head = head.trim().trim_end_matches('·').trim();
    let pct_text = if let Some(used) = head.strip_suffix("used") {
        used.trim().strip_suffix('%')?
    } else {
        // "... spent this month (21%)": the last parenthesised percentage.
        let open = head.rfind('(')?;
        head[open + 1..].trim_end_matches(')').trim().strip_suffix('%')?
    };
    let pct: f64 = pct_text.trim().parse().ok()?;
    if !pct.is_finite() || pct < 0.0 {
        return None;
    }
    let period = if head.contains("spent today") {
        Some(SpendPeriod::Daily)
    } else if head.contains("spent this week") {
        Some(SpendPeriod::Weekly)
    } else if head.contains("spent this month") {
        Some(SpendPeriod::Monthly)
    } else {
        None
    };
    Some(Spend { pct: pct.round().min(9999.0) as u32, resets_at: reset.and_then(|t| parse_reset(t, now)), period })
}

/// `" 15% used · resets Oct 13, 7am (Atlantic/Reykjavik)"` → Window.
fn parse_window(body: &str, now: DateTime<Utc>) -> Option<Window> {
    let pct_end = body.find('%')?;
    let pct: f64 = body[..pct_end].trim().parse().ok()?;
    if !pct.is_finite() || pct < 0.0 {
        return None;
    }
    let after = &body[pct_end + 1..];
    let resets_at = after.find("resets").and_then(|i| parse_reset(after[i + "resets".len()..].trim(), now));
    Some(Window { pct: pct.round().min(9999.0) as u32, resets_at })
}

const MONTHS: [&str; 12] = ["jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"];

/// Parse reset text such as `Oct 13, 9:59am (Atlantic/Reykjavik)`,
/// `Oct 13 at 7 PM (Europe/Berlin)`, `10pm (UTC)`, `Oct 13, 19:00 (UTC)` or
/// `Jan 2, 2027, 9:59pm (UTC)` (the CLI adds the year only when it isn't the
/// current one). With no year the next occurrence after `now` wins (a reset
/// that passed less than an hour ago is kept, so a lagging CLI reads as
/// "resets in 0m"). Without a zone the machine's local zone is used.
pub fn parse_reset(text: &str, now: DateTime<Utc>) -> Option<DateTime<Utc>> {
    let text = text.trim();
    let (when, zone) = match (text.rfind('('), text.rfind(')')) {
        (Some(a), Some(b)) if b > a => (text[..a].trim(), Some(text[a + 1..b].trim())),
        _ => (text, None),
    };
    let when = when.replace(" at ", ", ");
    if let Some((d, t)) = when.rsplit_once(',') {
        return parse_reset_parts(Some(d.trim()), t.trim(), zone, now);
    }
    // "Oct 13 7am" or a bare "7am" / "7 PM".
    let toks: Vec<&str> = when.split_whitespace().collect();
    if toks.len() >= 3 {
        return parse_reset_parts(Some(&toks[..2].join(" ")), &toks[2..].join(" "), zone, now);
    }
    parse_reset_parts(None, &when, zone, now)
}

fn parse_reset_parts(
    date_part: Option<&str>,
    time_part: &str,
    zone: Option<&str>,
    now: DateTime<Utc>,
) -> Option<DateTime<Utc>> {
    let time = parse_time(time_part)?;
    let md = match date_part {
        Some(d) if !d.is_empty() => Some(parse_date(d)?),
        _ => None,
    };
    match zone {
        Some(z) => {
            let tz: Tz = z.parse().ok()?;
            next_occurrence(&tz, md, time, now)
        }
        None => next_occurrence(&chrono::Local, md, time, now),
    }
}

/// `Oct 13` / `October 13` / `13 Oct`, optionally followed by `, 2027`.
fn parse_date(s: &str) -> Option<MonthDay> {
    if let Some((md, year)) = s.rsplit_once(',') {
        let year = year.trim();
        if year.len() == 4 && year.bytes().all(|b| b.is_ascii_digit()) {
            let (m, d) = parse_month_day(md.trim())?;
            return Some(MonthDay { month: m, day: d, year: year.parse().ok() });
        }
    }
    let (m, d) = parse_month_day(s)?;
    Some(MonthDay { month: m, day: d, year: None })
}

/// A reset date: month, day and (when the CLI printed one) the year.
#[derive(Debug, Clone, Copy)]
struct MonthDay {
    month: u32,
    day: u32,
    year: Option<i32>,
}

/// `Oct 13` / `October 13` / `13 Oct`.
fn parse_month_day(s: &str) -> Option<(u32, u32)> {
    let parts: Vec<&str> = s.split_whitespace().collect();
    if parts.len() != 2 {
        return None;
    }
    let month = |p: &str| {
        let p = p.trim_end_matches('.').to_ascii_lowercase();
        if p.len() < 3 {
            return None;
        }
        MONTHS.iter().position(|m| p.starts_with(m)).map(|i| i as u32 + 1)
    };
    let day = |p: &str| p.trim_end_matches(|c: char| c.is_ascii_alphabetic()).parse::<u32>().ok();
    match (month(parts[0]), day(parts[1])) {
        (Some(m), Some(d)) => Some((m, d)),
        _ => match (day(parts[0]), month(parts[1])) {
            (Some(d), Some(m)) => Some((m, d)),
            _ => None,
        },
    }
}

/// `7am`, `9:59am`, `7 PM`, `7:00 p.m.`, `19:00`.
fn parse_time(s: &str) -> Option<NaiveTime> {
    let s = s.trim().to_ascii_lowercase().replace('.', "");
    let s = s.replace(' ', "");
    let (digits, meridiem) = if let Some(d) = s.strip_suffix("am") {
        (d, Some(false))
    } else if let Some(d) = s.strip_suffix("pm") {
        (d, Some(true))
    } else {
        (s.as_str(), None)
    };
    let (h, m) = match digits.split_once(':') {
        Some((h, m)) => (h.parse::<u32>().ok()?, m.parse::<u32>().ok()?),
        None => (digits.parse::<u32>().ok()?, 0),
    };
    if m > 59 {
        return None;
    }
    let h = match meridiem {
        Some(pm) => {
            if !(1..=12).contains(&h) {
                return None;
            }
            (h % 12) + if pm { 12 } else { 0 }
        }
        None => {
            if h > 23 {
                return None;
            }
            h
        }
    };
    NaiveTime::from_hms_opt(h, m, 0)
}

/// Grace for a reset that has just passed (CLI lag): keep it rather than
/// jumping a year (or a day) ahead.
const PAST_GRACE_MINUTES: i64 = 60;

fn next_occurrence<Z: TimeZone>(
    tz: &Z,
    md: Option<MonthDay>,
    time: NaiveTime,
    now: DateTime<Utc>,
) -> Option<DateTime<Utc>> {
    let local_now = now.with_timezone(tz).naive_local();
    let floor = now - ChronoDuration::minutes(PAST_GRACE_MINUTES);
    let resolve = |date: NaiveDate| -> Option<DateTime<Utc>> {
        let naive = date.and_time(time);
        let local = tz
            .from_local_datetime(&naive)
            .earliest()
            // Nonexistent (DST gap): the wall clock jumps an hour forward.
            .or_else(|| tz.from_local_datetime(&(naive + ChronoDuration::hours(1))).earliest())?;
        Some(local.with_timezone(&Utc))
    };
    let candidates: Vec<NaiveDate> = match md {
        // An explicit year is taken as printed, past or not.
        Some(MonthDay { month, day, year: Some(y) }) => {
            return NaiveDate::from_ymd_opt(y, month, day).and_then(resolve);
        }
        Some(MonthDay { month, day, year: None }) => (local_now.year() - 1..=local_now.year() + 1)
            .filter_map(|y| NaiveDate::from_ymd_opt(y, month, day))
            .collect(),
        None => {
            let today = local_now.date();
            vec![today - ChronoDuration::days(1), today, today + ChronoDuration::days(1)]
        }
    };
    candidates.into_iter().filter_map(resolve).filter(|t| *t >= floor).min()
}

/// Locate the `claude` CLI: the configured path, then `PATH`, then the usual
/// install locations (daemons under systemd/launchd get a thin `PATH`), then
/// the newest copy the Claude desktop app downloaded for itself
/// ([`desktop_claude_root`], [`newest_bundled_claude`]).
pub fn find_claude(explicit: Option<&Path>, home: &Path) -> Option<PathBuf> {
    if explicit.is_some() {
        return find_cli(explicit, home);
    }
    find_cli(None, home).or_else(|| newest_bundled_claude(&desktop_claude_root(home)?))
}

/// Where the Claude desktop app keeps the Claude Code builds it downloads:
/// `<userData>/claude-code` (its `app.getPath("userData")`), i.e.
/// `$XDG_CONFIG_HOME/Claude` or `~/.config/Claude` on Linux,
/// `~/Library/Application Support/Claude` on macOS, `%APPDATA%\Claude` on
/// Windows.
pub fn desktop_claude_root(home: &Path) -> Option<PathBuf> {
    let user_data = if cfg!(target_os = "macos") {
        home.join("Library/Application Support/Claude")
    } else if cfg!(windows) {
        PathBuf::from(std::env::var_os("APPDATA")?).join("Claude")
    } else {
        std::env::var_os("XDG_CONFIG_HOME")
            .map(PathBuf::from)
            .filter(|p| p.is_absolute())
            .unwrap_or_else(|| home.join(".config"))
            .join("Claude")
    };
    Some(user_data.join("claude-code"))
}

/// The newest runnable Claude Code under the desktop app's `claude-code`
/// dir. Builds live at `<version>/<checksum>/claude` (`claude.exe` on
/// Windows; on macOS also `claude.app/Contents/MacOS/claude`). Versions sort
/// numerically; a version with no runnable binary is skipped.
pub fn newest_bundled_claude(root: &Path) -> Option<PathBuf> {
    let mut versions: Vec<(Vec<u64>, PathBuf)> = std::fs::read_dir(root)
        .ok()?
        .filter_map(Result::ok)
        .filter_map(|e| {
            let name = e.file_name().into_string().ok()?;
            let v: Vec<u64> = name.split('.').map(|p| p.parse().ok()).collect::<Option<_>>()?;
            Some((v, e.path()))
        })
        .collect();
    versions.sort_by(|a, b| b.0.cmp(&a.0));
    let names: &[&str] = if cfg!(windows) {
        &["claude.exe"]
    } else if cfg!(target_os = "macos") {
        &["claude", "claude.app/Contents/MacOS/claude"]
    } else {
        &["claude"]
    };
    versions.into_iter().find_map(|(_, dir)| {
        let mut builds: Vec<PathBuf> =
            std::fs::read_dir(&dir).ok()?.filter_map(Result::ok).map(|e| e.path()).filter(|p| p.is_dir()).collect();
        builds.sort();
        builds.push(dir);
        builds.iter().flat_map(|b| names.iter().map(move |n| b.join(n))).find(|p| is_executable(p))
    })
}

fn is_executable(p: &Path) -> bool {
    let Ok(m) = std::fs::metadata(p) else { return false };
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        m.is_file() && m.permissions().mode() & 0o111 != 0
    }
    #[cfg(not(unix))]
    {
        m.is_file()
    }
}

/// The desktop app's own copy of Claude Code (under [`desktop_claude_root`]).
pub fn is_desktop_bundled(claude: &Path, home: &Path) -> bool {
    desktop_claude_root(home).is_some_and(|r| claude.starts_with(r))
}

fn find_cli(explicit: Option<&Path>, home: &Path) -> Option<PathBuf> {
    if let Some(p) = explicit {
        return p.is_file().then(|| p.to_path_buf());
    }
    let names: &[&str] = if cfg!(windows) { &["claude.exe", "claude.cmd", "claude"] } else { &["claude"] };
    if let Some(path) = std::env::var_os("PATH") {
        for dir in std::env::split_paths(&path) {
            for n in names {
                let c = dir.join(n);
                if c.is_file() {
                    return Some(c);
                }
            }
        }
    }
    let mut extra = vec![
        home.join(".local/bin"),
        home.join(".claude/local"),
        home.join(".npm-global/bin"),
        home.join(".bun/bin"),
        home.join(".volta/bin"),
        PathBuf::from("/opt/homebrew/bin"),
        PathBuf::from("/usr/local/bin"),
        PathBuf::from("/usr/bin"),
    ];
    if cfg!(windows) {
        if let Some(appdata) = std::env::var_os("APPDATA") {
            extra.push(PathBuf::from(appdata).join("npm"));
        }
        extra.push(home.join(".local\\bin"));
    }
    extra.into_iter().flat_map(|d| names.iter().map(move |n| d.join(n))).find(|c| c.is_file())
}

/// Default per-call timeout for the CLI (it answers in about a second).
pub const CLI_TIMEOUT: Duration = Duration::from_secs(30);

/// Point a CLI command at one account. `~/.claude` (the CLI's own default)
/// runs with `CLAUDE_CONFIG_DIR` unset, because setting it changes where the
/// CLI looks for `.claude.json` and which Keychain item it reads on macOS.
pub(crate) fn for_account(cmd: &mut Command, config_dir: &Path, home: &Path) {
    cmd.stdin(Stdio::null()).stdout(Stdio::piped()).stderr(Stdio::piped());
    if config_dir == home.join(".claude") {
        cmd.env_remove("CLAUDE_CONFIG_DIR");
    } else {
        cmd.env("CLAUDE_CONFIG_DIR", config_dir);
    }
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        const CREATE_NO_WINDOW: u32 = 0x0800_0000;
        cmd.creation_flags(CREATE_NO_WINDOW);
    }
}

/// Build the `/usage` command for one account.
pub fn usage_command(claude: &Path, config_dir: &Path, home: &Path) -> Command {
    let mut cmd = Command::new(claude);
    cmd.args(["--output-format", "json", "-p", "/usage"]);
    for_account(&mut cmd, config_dir, home);
    cmd
}

/// Run `/usage` for one account (blocking) and return the CLI's stdout.
pub fn run_usage_cli(claude: &Path, config_dir: &Path, home: &Path, timeout: Duration) -> Result<String, UsageError> {
    run_cli(usage_command(claude, config_dir, home), timeout)
}

/// Run a prepared CLI command (blocking, killed after `timeout`) and return
/// its stdout. A failed exit with no JSON on stdout is an auth problem when
/// the CLI says so, else [`UsageError::BadOutput`].
pub(crate) fn run_cli(mut cmd: Command, timeout: Duration) -> Result<String, UsageError> {
    let mut child = cmd.spawn().map_err(|e| UsageError::Spawn(e.to_string()))?;
    let mut stdout = child.stdout.take().expect("piped");
    let mut stderr = child.stderr.take().expect("piped");
    let out_reader = std::thread::spawn(move || {
        let mut s = String::new();
        let _ = stdout.read_to_string(&mut s);
        s
    });
    let err_reader = std::thread::spawn(move || {
        let mut s = String::new();
        let _ = stderr.read_to_string(&mut s);
        s
    });
    let start = Instant::now();
    let status = loop {
        match child.try_wait() {
            Ok(Some(st)) => break st,
            Ok(None) if start.elapsed() >= timeout => {
                let _ = child.kill();
                let _ = child.wait();
                return Err(UsageError::Timeout);
            }
            Ok(None) => std::thread::sleep(Duration::from_millis(50)),
            Err(e) => return Err(UsageError::Spawn(e.to_string())),
        }
    };
    let out = out_reader.join().unwrap_or_default();
    let err = err_reader.join().unwrap_or_default();
    if !status.success() && !out.contains('{') {
        if let Some(e) = classify_auth(&err).or_else(|| classify_auth(&out)) {
            return Err(e);
        }
        return Err(UsageError::BadOutput(format!("exit {status}: {}", snippet(&err))));
    }
    Ok(out)
}

/// Run and parse `/usage` for one account. A reply with no usage lines is
/// checked against Claude Code's debug log ([`rate_limit_from_debug_log`]).
pub fn poll_account(
    claude: &Path,
    config_dir: &Path,
    home: &Path,
    now: DateTime<Utc>,
) -> Result<UsageReading, UsageError> {
    let out = run_usage_cli(claude, config_dir, home, CLI_TIMEOUT)?;
    match parse_cli_output(&out, now) {
        Err(UsageError::NoUsageLines(text)) => Err(match rate_limit_from_debug_log(config_dir, Utc::now()) {
            Some(retry_after_secs) => UsageError::RateLimited { retry_after_secs },
            None => UsageError::NoUsageLines(text),
        }),
        other => other,
    }
}

/// How much of the debug log's end is read.
const DEBUG_TAIL_BYTES: u64 = 256 * 1024;

/// Best effort: Claude Code's newest debug log for this config dir
/// (`<config_dir>/debug/latest`, else the newest `*.txt` there) says its
/// usage endpoint answered 429 and is still being left alone. Returns the
/// seconds left. Claude Code writes this log only in some modes; we never ask
/// it to (no `--debug`), so a missing log is the normal case.
pub fn rate_limit_from_debug_log(config_dir: &Path, now: DateTime<Utc>) -> Option<i64> {
    let dir = config_dir.join("debug");
    let latest = dir.join("latest");
    let file = if latest.is_file() {
        latest
    } else {
        std::fs::read_dir(&dir)
            .ok()?
            .filter_map(Result::ok)
            .filter(|e| e.path().extension().is_some_and(|x| x == "txt"))
            .filter_map(|e| Some((e.metadata().ok()?.modified().ok()?, e.path())))
            .max_by_key(|(t, _)| *t)?
            .1
    };
    let tail = read_tail(&file, DEBUG_TAIL_BYTES).ok()?;
    rate_limit_from_debug_text(&tail, now)
}

fn read_tail(path: &Path, max: u64) -> std::io::Result<String> {
    use std::io::{Seek, SeekFrom};
    let mut f = std::fs::File::open(path)?;
    let len = f.metadata()?.len();
    f.seek(SeekFrom::Start(len.saturating_sub(max)))?;
    let mut buf = Vec::new();
    f.read_to_end(&mut buf)?;
    Ok(String::from_utf8_lossy(&buf).into_owned())
}

/// The newest 429 report in debug-log text, as seconds left at `now`:
///
/// ```text
/// 2026-10-13T15:47:55.994Z [DEBUG] fetchUtilization: 429 remembered for this bearer; not asking again for 589s
/// 2026-10-13T15:47:55.994Z [ERROR] Failed to load usage data: Request failed with status code 429
/// ```
///
/// The first form gives the remaining time; the second alone counts as a
/// fresh 429 with [`crate::backoff::RATE_LIMIT_FLOOR_SECS`] to go. Lines
/// whose memory has run out by `now`, or without a timestamp, are ignored.
pub fn rate_limit_from_debug_text(text: &str, now: DateTime<Utc>) -> Option<i64> {
    let left_at = |line: &str, secs: i64| -> Option<i64> {
        let ts = DateTime::parse_from_rfc3339(line.split_whitespace().next()?).ok()?;
        Some((ts.with_timezone(&Utc) + ChronoDuration::seconds(secs) - now).num_seconds())
    };
    let remembered = text.lines().rev().find_map(|line| {
        if !(line.contains("fetchUtilization") && line.contains("429")) {
            return None;
        }
        let rest = line.split("not asking again for ").nth(1)?;
        let digits: String = rest.chars().take_while(char::is_ascii_digit).collect();
        left_at(line, digits.parse().ok()?)
    });
    let left = remembered.or_else(|| {
        text.lines()
            .rev()
            .filter(|l| l.contains("Failed to load usage data") && l.contains("429"))
            .find_map(|l| left_at(l, crate::backoff::RATE_LIMIT_FLOOR_SECS))
    })?;
    (left > 0).then_some(left)
}

/// Consecutive misses after which an account's numbers count as stale.
pub const STALE_AFTER_MISSES: u32 = 3;

/// Age of the last good reading after which a missing account counts as stale,
/// however few misses that took: with the rate-limit backoff, three misses can
/// take an hour, far too long to show old numbers as live.
pub const STALE_AFTER_SECS: i64 = 20 * 60;

/// Per-account memory across polls: last good reading, its age, misses and
/// the backoff they caused.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct AccountState {
    /// Last successful reading, kept through misses.
    pub last: Option<UsageReading>,
    /// When `last` was read (unix seconds).
    pub last_ok_at: Option<i64>,
    /// Consecutive failed polls since `last_ok_at`.
    pub misses: u32,
    /// The most recent error, for the tray.
    pub last_error: Option<UsageError>,
    /// When a poll of this account last started (unix seconds).
    pub last_attempt_at: Option<i64>,
    /// Pause after transient misses (rate limiting).
    pub backoff: Backoff,
    /// When the poll that produced `last` started (unix seconds): what it
    /// says was true then.
    pub read_started_at: Option<i64>,
    /// The first prompt or tool activity from this account after
    /// [`crate::probe::anchor`]: the user's own activity started a window
    /// there.
    pub first_activity: Option<i64>,
    /// Consecutive probe misses (see [`crate::probe::PROBE_MISS_LIMIT`]).
    pub probe_misses: u32,
}

impl AccountState {
    /// Record one [`crate::probe::Polled`]: an idle-source result as
    /// [`AccountState::record`]; a probe's own miss counts as a miss but
    /// doesn't back off the idle source, whose rate limit it never touched.
    pub fn record_polled(&mut self, now: i64, polled: crate::probe::Polled) {
        if polled.probe_missed {
            self.probe_misses = self.probe_misses.saturating_add(1);
        } else if !polled.idle && polled.result.is_ok() {
            self.probe_misses = 0;
        }
        match polled.result {
            Err(e) if !polled.idle => {
                self.misses = self.misses.saturating_add(1);
                self.last_error = Some(e);
            }
            r => self.record(now, r),
        }
    }

    /// Record a poll outcome. A transient miss starts or extends the
    /// account's backoff; a good reading clears it.
    pub fn record(&mut self, now: i64, result: Result<UsageReading, UsageError>) {
        match result {
            Ok(r) => {
                self.last = Some(r);
                self.last_ok_at = Some(now);
                self.read_started_at = Some(self.last_attempt_at.unwrap_or(now).min(now));
                let anchor = crate::probe::anchor(self);
                self.first_activity = self.first_activity.filter(|t| anchor.is_some_and(|a| *t > a));
                if self.first_activity.is_none() {
                    self.probe_misses = 0;
                }
                self.misses = 0;
                self.last_error = None;
                self.backoff.on_success();
            }
            Err(e) => {
                self.misses = self.misses.saturating_add(1);
                match e {
                    // The endpoint said exactly how long: no more, no less.
                    UsageError::Throttled { retry_after_secs: Some(s) } => self.backoff.park_exact(now, s),
                    // A refused token is retried on the ladder too, not
                    // every poll.
                    _ if e.is_transient() || e == UsageError::TokenRejected => {
                        self.backoff.on_miss(now, e.retry_after());
                    }
                    _ => {}
                }
                self.last_error = Some(e);
            }
        }
    }

    /// Three misses in a row (or never read at all after three tries), or a
    /// miss with the last good reading over [`STALE_AFTER_SECS`] old.
    pub fn is_stale(&self, now: i64) -> bool {
        self.misses >= STALE_AFTER_MISSES || (self.misses > 0 && self.age(now).is_some_and(|a| a > STALE_AFTER_SECS))
    }

    /// Note prompt or tool activity from this account at `ts`.
    pub fn note_activity(&mut self, ts: i64) {
        if self.first_activity.is_none() && crate::probe::anchor(self).is_some_and(|a| ts > a) {
            self.first_activity = Some(ts);
        }
    }

    /// Seconds since the last good reading.
    pub fn age(&self, now: i64) -> Option<i64> {
        self.last_ok_at.map(|t| (now - t).max(0))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn at(s: &str) -> DateTime<Utc> {
        DateTime::parse_from_rfc3339(s).unwrap().with_timezone(&Utc)
    }

    #[cfg(unix)]
    #[test]
    fn finds_the_newest_desktop_bundled_claude() {
        use std::os::unix::fs::PermissionsExt;
        let root = std::env::temp_dir().join(format!("cm-bundled-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        let put = |rel: &str, exec: bool| {
            let p = root.join(rel);
            std::fs::create_dir_all(p.parent().unwrap()).unwrap();
            std::fs::write(&p, "#!/bin/sh\n").unwrap();
            std::fs::set_permissions(&p, std::fs::Permissions::from_mode(if exec { 0o755 } else { 0o644 })).unwrap();
            p
        };
        assert_eq!(newest_bundled_claude(&root), None, "no dir");
        let old = put("2.1.288/dc67a1d84fec/claude", true);
        put("2.1.95/aaaaaaaaaaaa/claude", true);
        assert_eq!(newest_bundled_claude(&root), Some(old.clone()), "2.1.288 beats 2.1.95");
        // A newer version without a runnable binary is skipped; junk dirs too.
        put("2.1.300/bbbbbbbbbbbb/claude", false);
        std::fs::create_dir_all(root.join("2.1.301/cccccccccccc")).unwrap();
        std::fs::create_dir_all(root.join("latest")).unwrap();
        put("10.0.0-beta/dddddddddddd/claude", true);
        assert_eq!(newest_bundled_claude(&root), Some(old));
        let new = put("2.1.295/71164c85f9d2/claude", true);
        assert_eq!(newest_bundled_claude(&root), Some(new));
        let _ = std::fs::remove_dir_all(&root);
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn desktop_root_follows_xdg_config_home() {
        let home = Path::new("/home/me");
        // Only checks the default here: the test must not mutate the
        // process environment other tests read.
        if std::env::var_os("XDG_CONFIG_HOME").is_none() {
            assert_eq!(desktop_claude_root(home), Some(PathBuf::from("/home/me/.config/Claude/claude-code")));
        }
        assert!(is_desktop_bundled(&desktop_claude_root(home).unwrap().join("2.1.295/71164c85f9d2/claude"), home));
        assert!(!is_desktop_bundled(Path::new("/home/me/.local/bin/claude"), home));
    }

    #[test]
    fn reset_basic_formats() {
        let now = at("2026-10-13T05:40:00Z");
        assert_eq!(parse_reset("Oct 13, 7am (Atlantic/Reykjavik)", now), Some(at("2026-10-13T07:00:00Z")));
        assert_eq!(parse_reset("Oct 13, 9:59am (Atlantic/Reykjavik)", now), Some(at("2026-10-13T09:59:00Z")));
        assert_eq!(parse_reset("Oct 18, 10pm (Atlantic/Reykjavik)", now), Some(at("2026-10-18T22:00:00Z")));
        assert_eq!(parse_reset("Oct 13, 12am (UTC)", now), Some(at("2027-10-13T00:00:00Z")));
        assert_eq!(parse_reset("Oct 13, 12pm (UTC)", now), Some(at("2026-10-13T12:00:00Z")));
        assert_eq!(parse_reset("Oct 13 at 7 PM (Europe/Berlin)", now), Some(at("2026-10-13T17:00:00Z")));
        assert_eq!(parse_reset("Oct 13, 19:00 (America/New_York)", now), Some(at("2026-10-13T23:00:00Z")));
        assert_eq!(parse_reset("October 14, 2:59 p.m. (Atlantic/Reykjavik)", now), Some(at("2026-10-14T14:59:00Z")));
    }

    #[test]
    fn reset_year_rollover_and_grace() {
        // Dec 31 local, reset "Jan 1" is next year.
        let now = at("2026-12-31T20:00:00Z");
        assert_eq!(parse_reset("Jan 1, 3am (UTC)", now), Some(at("2027-01-01T03:00:00Z")));
        // Just passed (CLI lag) stays, does not jump a year.
        let now = at("2026-10-13T07:10:00Z");
        assert_eq!(parse_reset("Oct 13, 7am (UTC)", now), Some(at("2026-10-13T07:00:00Z")));
        // Long passed: next year.
        let now = at("2026-10-13T12:00:00Z");
        assert_eq!(parse_reset("Oct 13, 7am (UTC)", now), Some(at("2027-10-13T07:00:00Z")));
    }

    #[test]
    fn reset_time_only_and_zone_offsets() {
        let now = at("2026-10-13T20:00:00Z");
        assert_eq!(parse_reset("10pm (UTC)", now), Some(at("2026-10-13T22:00:00Z")));
        assert_eq!(parse_reset("3am (UTC)", now), Some(at("2026-10-14T03:00:00Z")));
        // Zone east of UTC: local date differs from UTC date.
        let now = at("2026-10-13T22:30:00Z"); // already Oct 14 in Tokyo
        assert_eq!(parse_reset("Oct 14, 9am (Asia/Tokyo)", now), Some(at("2026-10-14T00:00:00Z")));
    }

    #[test]
    fn reset_dst_gap_and_bad_input() {
        let now = at("2026-03-01T00:00:00Z");
        // 2:30am on Mar 29 doesn't exist in Berlin; take 3:30 CEST = 01:30Z.
        assert_eq!(parse_reset("Mar 29, 2:30am (Europe/Berlin)", now), Some(at("2026-03-29T01:30:00Z")));
        assert_eq!(parse_reset("Oct 13, 7am (Not/AZone)", now), None);
        assert_eq!(parse_reset("someday", now), None);
        assert_eq!(parse_reset("Oct 13, 13pm (UTC)", now), None);
        assert_eq!(parse_reset("Feb 30, 1am (UTC)", now), None);
    }

    #[test]
    fn reset_with_explicit_year() {
        // The CLI prints the year only when it isn't the current one.
        let now = at("2026-12-30T12:00:00Z");
        assert_eq!(parse_reset("Jan 2, 2027, 9:59pm (UTC)", now), Some(at("2027-01-02T21:59:00Z")));
        assert_eq!(parse_reset("Jan 2, 2027, 3am (Asia/Tokyo)", now), Some(at("2027-01-01T18:00:00Z")));
        // Taken as printed even when it lies in the past.
        assert_eq!(parse_reset("Dec 1, 2025, 1am (UTC)", now), Some(at("2025-12-01T01:00:00Z")));
        assert_eq!(parse_reset("Jan 2, 27, 1am (UTC)", now), None);
    }

    // /usage reports a spend cap as a "Spend limit:" line.
    #[test]
    fn spend_limit_lines() {
        let now = at("2026-10-13T05:40:00Z");
        let r =
            parse_usage_text("Spend limit: $41.20 / $200.00 spent this month (21%) · resets Nov 1, 12am (UTC)", now)
                .unwrap();
        assert!(r.is_spend_only());
        assert_eq!(
            r.spend,
            Some(Spend { pct: 21, resets_at: Some(at("2026-11-01T00:00:00Z")), period: Some(SpendPeriod::Monthly) })
        );
        let r = parse_usage_text("Spend limit: 104% used · resets Nov 1, 12am (UTC)", now).unwrap();
        assert_eq!(r.spend.as_ref().map(|s| (s.pct, s.period)), Some((104, None)));
        assert!(r.limited());
        let r = parse_usage_text("Spend limit: €3,00 / €10,00 spent today (30%)", now).unwrap();
        assert_eq!(r.spend, Some(Spend { pct: 30, resets_at: None, period: Some(SpendPeriod::Daily) }));
        let r =
            parse_usage_text("Spend limit: $1 / $10 spent this week (10%) · resets Oct 19, 12am (UTC)", now).unwrap();
        assert_eq!(r.spend.unwrap().period, Some(SpendPeriod::Weekly));
        assert!(matches!(parse_usage_text("Spend limit: lots", now), Err(UsageError::NoUsageLines(_))));
    }

    #[test]
    fn overage_and_limited() {
        let now = at("2026-10-13T05:40:00Z");
        let full = "Current session: 100% used · resets Oct 13, 7am (UTC)\nCurrent week (all models): 3% used";
        let r = parse_usage_text(full, now).unwrap();
        assert!(!r.overage);
        assert!(r.limited());
        let r = parse_usage_text(&format!("You are currently using your overages to power it\n\n{full}"), now).unwrap();
        assert!(r.overage);
        assert!(!r.limited(), "extra usage is covering the requests");
        let week_full = "Current session: 3% used\nCurrent week (all models): 100% used · resets Oct 18, 10pm (UTC)";
        assert!(parse_usage_text(week_full, now).unwrap().limited());
    }

    #[test]
    fn account_state_staleness() {
        let mut a = AccountState::default();
        a.record(100, Ok(UsageReading::default()));
        assert!(!a.is_stale(100));
        a.record(160, Err(UsageError::Timeout));
        a.record(220, Err(UsageError::Timeout));
        assert!(!a.is_stale(220));
        assert!(a.last.is_some(), "a miss keeps the last values");
        assert_eq!(a.age(220), Some(120));
        a.record(280, Err(UsageError::NoUsageLines("x".into())));
        assert!(a.is_stale(280));
        a.record(340, Ok(UsageReading::default()));
        assert!(!a.is_stale(340));
    }

    #[test]
    fn one_miss_goes_stale_once_the_reading_is_old() {
        let mut a = AccountState::default();
        a.record(0, Ok(UsageReading::default()));
        a.record(600, Err(UsageError::NoUsageLines("x".into())));
        assert!(!a.is_stale(600));
        assert!(!a.is_stale(STALE_AFTER_SECS));
        assert!(a.is_stale(STALE_AFTER_SECS + 1));
        // No miss yet: an old reading alone isn't stale (polls are just spaced out).
        let mut b = AccountState::default();
        b.record(0, Ok(UsageReading::default()));
        assert!(!b.is_stale(STALE_AFTER_SECS * 3));
    }

    // A miss that may have reached the endpoint parks the account; auth
    // problems and a missing CLI don't.
    #[test]
    fn account_state_backoff() {
        let mut a = AccountState::default();
        a.record(1000, Err(UsageError::NoUsageLines("x".into())));
        assert_eq!(a.backoff.until(), Some(1600));
        a.record(1600, Err(UsageError::RateLimited { retry_after_secs: 300 }));
        assert_eq!(a.backoff.until(), Some(2800), "second miss: 20 min");
        a.record(2800, Ok(UsageReading::default()));
        assert!(!a.backoff.active(2800));
        a.record(2900, Err(UsageError::NotLoggedIn("x".into())));
        a.record(2950, Err(UsageError::Spawn("x".into())));
        assert_eq!(a.backoff.until(), None);
    }

    #[test]
    fn paused_until_clock_time() {
        // 2026-10-14T01:19:13Z + 3600 s, shown in UTC and at UTC+2.
        assert_eq!(paused_message(1_791_940_753 + 3600, 0), "Usage paused by Anthropic until 02:19.");
        assert_eq!(paused_message(1_791_940_753 + 3600, 7200), "Usage paused by Anthropic until 04:19.");
    }

    #[test]
    fn user_messages_name_the_wait() {
        let rl = UsageError::NoUsageLines("".into());
        assert_eq!(rl.user_message(Some(590)), "No usage data was returned. Trying again in about 10 minutes.");
        assert_eq!(
            UsageError::RateLimited { retry_after_secs: 30 }.user_message(Some(45)),
            "Usage requests are temporarily limited. Trying again in about 1 minute."
        );
        assert_eq!(rl.user_message(None), "No usage data was returned. Retrying.");
        assert_eq!(
            UsageError::Timeout.user_message(Some(1200)),
            "Claude Code took too long to answer. Trying again in about 20 minutes."
        );
        assert!(UsageError::Timeout.is_transient() && !UsageError::Spawn("x".into()).is_transient());
        assert!(!UsageError::NotSubscription.is_transient());
    }

    const DEBUG_429: &str = "\
2026-10-13T15:47:55.974Z [DEBUG] fetchUtilization: GET /api/oauth/usage (attempt 1)
2026-10-13T15:47:55.994Z [DEBUG] fetchUtilization: 429 remembered for this bearer; not asking again for 589s
2026-10-13T15:47:55.994Z [ERROR] Failed to load usage data: Request failed with status code 429
2026-10-13T15:47:56.100Z [DEBUG] something else
";

    #[test]
    fn debug_log_rate_limit() {
        let now = at("2026-10-13T15:48:05Z");
        // 589 s from 15:47:55.994 is 15:57:44.994; 579 s left at 15:48:05.
        assert_eq!(rate_limit_from_debug_text(DEBUG_429, now), Some(579));
        // Expired memory: nothing.
        assert_eq!(rate_limit_from_debug_text(DEBUG_429, at("2026-10-13T16:00:00Z")), None);
        // Only the error line: a fresh 429 with the 600 s floor.
        let only_err = "2026-10-13T15:47:55Z [ERROR] Failed to load usage data: Request failed with status code 429\n";
        assert_eq!(rate_limit_from_debug_text(only_err, now), Some(590));
        // Unrelated logs and lines without a timestamp.
        assert_eq!(rate_limit_from_debug_text("[DEBUG] fetchUtilization: 429 not asking again for 500s\n", now), None);
        assert_eq!(rate_limit_from_debug_text("2026-10-13T15:47:55Z [DEBUG] fetchUtilization: 200 OK\n", now), None);
        assert_eq!(rate_limit_from_debug_text("", now), None);
    }

    #[test]
    fn debug_log_file_lookup() {
        let root = std::env::temp_dir().join(format!("clawdmeter-debuglog-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        let now = at("2026-10-13T15:48:05Z");
        assert_eq!(rate_limit_from_debug_log(&root, now), None, "no debug dir");
        let dbg = root.join("debug");
        std::fs::create_dir_all(&dbg).unwrap();
        std::fs::write(dbg.join("old.txt"), "2026-10-13T15:40:00Z [DEBUG] fine\n").unwrap();
        assert_eq!(rate_limit_from_debug_log(&root, now), None);
        std::thread::sleep(Duration::from_millis(20));
        std::fs::write(dbg.join("new.txt"), DEBUG_429).unwrap();
        assert_eq!(rate_limit_from_debug_log(&root, now), Some(579), "newest .txt without a latest link");
        std::fs::write(dbg.join("latest"), "2026-10-13T15:40:00Z [DEBUG] fine\n").unwrap();
        assert_eq!(rate_limit_from_debug_log(&root, now), None, "latest wins over mtimes");
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn command_env_for_default_and_extra_dirs() {
        let home = Path::new("/home/u");
        let c = usage_command(Path::new("/bin/claude"), &home.join(".claude"), home);
        let envs: Vec<_> = c.get_envs().collect();
        assert!(envs.iter().any(|(k, v)| *k == "CLAUDE_CONFIG_DIR" && v.is_none()));
        let c = usage_command(Path::new("/bin/claude"), &home.join(".claude-work"), home);
        let envs: Vec<_> = c.get_envs().collect();
        assert!(envs
            .iter()
            .any(|(k, v)| *k == "CLAUDE_CONFIG_DIR" && *v == Some(home.join(".claude-work").as_os_str())));
        let args: Vec<_> = c.get_args().collect();
        assert_eq!(args, ["--output-format", "json", "-p", "/usage"]);
    }
}
