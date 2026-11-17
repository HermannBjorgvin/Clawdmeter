//! OpenAI Codex usage, a trial: a Codex home (`~/.codex`, `$CODEX_HOME`, or
//! a folder added on the Accounts page) is an account like a Claude config
//! dir, shown on the device when its meter moved most recently.
//!
//! A folder is a Codex home when it holds `auth.json` ([`is_codex_dir`]).
//! The source is the endpoint the Codex CLI itself reads,
//! `GET https://chatgpt.com/backend-api/wham/usage`, with the CLI's stored
//! ChatGPT sign-in (`Authorization: Bearer <tokens.access_token>`,
//! `ChatGPT-Account-Id: <tokens.account_id>`). The live $100 Pro plan answers:
//!
//! ```text
//! {"plan_type": "prolite",
//!  "rate_limit": {"allowed": true, "limit_reached": false,
//!    "primary_window": {"used_percent": 0, "limit_window_seconds": 604800,
//!                       "reset_after_seconds": 508639, "reset_at": 1795303084},
//!    "secondary_window": null},
//!  "additional_rate_limits": null,
//!  "rate_limit_reset_credits": {"available_count": 2, "applicable_available_count": 0}, …}
//! ```
//!
//! The rules, each of which once put a wrong number on a device:
//!
//! * **The token is only read, never refreshed**: the Codex CLI owns it. An
//!   expired one isn't sent; a refused one (401/403) is no data.
//! * **The endpoint is slow** (3–4 s healthy, over 10 s in bad spells), so
//!   the timeout is generous ([`HTTP_TIMEOUT`]): a short one would quietly
//!   hand every poll to the fallback.
//! * **The account limit, never a model's.** `rate_limit` is the plan's
//!   limit; `additional_rate_limits` holds per-model buckets (Spark …) that
//!   are never read. In the session logs only `limit_id: "codex"` counts.
//! * **Windows by duration.** `primary` / `secondary` are positional: a
//!   weekly-only plan puts its week in `primary`. A 5-hour window is the
//!   session, a 7-day one the week ([`classify`]).
//! * **The session-log fallback refuses stale data.** When the endpoint
//!   misses, the newest account record in `<home>/sessions/**/rollout-*.jsonl`
//!   stands in, but only when it is under an hour old and none of its
//!   windows has reset since ([`reading_from_log`]); otherwise no data.
//!
//! Reset credits: the usage endpoint gives the count held. When it changes,
//! or hourly, one more read-only GET the CLI uses,
//! `wham/rate-limit-reset-credits`, says when each held credit expires.
//! Nothing here ever consumes a credit.

use std::collections::HashMap;
use std::io::{Read, Seek, SeekFrom};
use std::path::{Path, PathBuf};
use std::time::Duration;

use chrono::{DateTime, Utc};
use serde_json::Value;

use crate::probe::{Polled, ProbeRun};
use crate::usage::{Provider, ResetCredits, SessionLimit, UsageError, UsageReading, Window};

/// The usage endpoint.
pub const USAGE_URL: &str = "https://chatgpt.com/backend-api/wham/usage";
/// Held reset credits, with when each expires.
pub const CREDITS_URL: &str = "https://chatgpt.com/backend-api/wham/rate-limit-reset-credits";
/// Reset credits granted and used in the provider's trailing window.
/// Timeout for each call: the endpoint is slow, and a miss falls back.
pub const HTTP_TIMEOUT: Duration = Duration::from_secs(30);
/// The account's own limit in the session logs (`rate_limits.limit_id`).
pub const ACCOUNT_LIMIT_ID: &str = "codex";
/// The oldest session-log record that may stand in for the endpoint (s).
pub const LOG_MAX_AGE_SECS: i64 = 3600;
/// How long the reset-credit details are kept before asking again (s),
/// unless the held count changes.
pub const CREDITS_TTL_SECS: i64 = 3600;
/// Bytes read from the end of a session log: its newest records.
const LOG_TAIL_BYTES: u64 = 512 * 1024;

/// A shown Codex account is polled this often (s).
pub const SHOWN_EVERY_SECS: i64 = 300;
/// Other Codex accounts are polled this often (s), like other Claude accounts.
pub const OTHER_EVERY_SECS: i64 = 600;
/// No Codex account is polled twice within this (s), whatever asked.
pub const MIN_GAP_SECS: i64 = 120;

/// The folder holds a Codex sign-in, `auth.json`: a Codex home, not a
/// Claude config dir.
pub fn is_codex_dir(dir: &Path) -> bool {
    dir.join("auth.json").is_file()
}

/// The Codex CLI's home: `$CODEX_HOME` when set, else `~/.codex`.
pub fn default_dir(home: &Path, codex_home: Option<&str>) -> PathBuf {
    match codex_home.map(str::trim).filter(|s| !s.is_empty()) {
        Some("~") => home.to_path_buf(),
        Some(d) => d.strip_prefix("~/").map_or_else(|| PathBuf::from(d), |rest| home.join(rest)),
        None => home.join(".codex"),
    }
}

/// The default Codex home, when it is one ([`is_codex_dir`]).
pub fn discover(home: &Path, codex_home: Option<&str>) -> Option<PathBuf> {
    Some(default_dir(home, codex_home)).filter(|d| is_codex_dir(d))
}

/// What a usage window measures, by its length.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum WindowKind {
    /// The 5-hour window.
    Session,
    /// The 7-day window.
    Week,
}

/// A window `secs` long: 4–6 h is the session, 6–8 days the week, anything
/// else isn't read.
pub fn classify(secs: i64) -> Option<WindowKind> {
    match secs {
        14_400..=21_600 => Some(WindowKind::Session),
        518_400..=691_200 => Some(WindowKind::Week),
        _ => None,
    }
}

/// The Codex CLI's stored ChatGPT sign-in.
#[derive(Clone, PartialEq, Eq)]
pub struct Auth {
    /// The bearer token.
    pub access: String,
    /// `ChatGPT-Account-Id`, when stored.
    pub account_id: Option<String>,
    /// The token's `exp` (unix seconds), when readable.
    pub expires_at: Option<i64>,
}

impl std::fmt::Debug for Auth {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Auth")
            .field("access", &"<redacted>")
            .field("account_id", &self.account_id.as_ref().map(|_| "<redacted>"))
            .field("expires_at", &self.expires_at)
            .finish()
    }
}

impl Auth {
    /// Expired at `now` (unix seconds), with a minute's slack.
    pub fn expired(&self, now: i64) -> bool {
        self.expires_at.is_some_and(|t| t <= now + 60)
    }
}

/// Read `auth.json`'s text. A file without ChatGPT tokens (an API-key
/// sign-in) has no usage limits to read.
pub fn parse_auth(text: &str) -> Result<Auth, UsageError> {
    let v: Value = serde_json::from_str(text.trim())
        .map_err(|_| UsageError::NotLoggedIn("Codex's auth.json isn't readable".into()))?;
    let tokens = v.get("tokens").filter(|t| t.is_object());
    let access = tokens
        .and_then(|t| t.get("access_token"))
        .and_then(Value::as_str)
        .filter(|s| !s.is_empty())
        .ok_or_else(|| UsageError::NotLoggedIn("Codex isn't signed in with ChatGPT".into()))?;
    let account_id =
        tokens.and_then(|t| t.get("account_id")).and_then(Value::as_str).filter(|s| !s.is_empty()).map(str::to_owned);
    Ok(Auth { access: access.to_owned(), account_id, expires_at: jwt_exp(access) })
}

/// `<dir>/auth.json`, read.
pub fn read_auth(dir: &Path) -> Result<Auth, UsageError> {
    let text = std::fs::read_to_string(dir.join("auth.json"))
        .map_err(|_| UsageError::NotLoggedIn("no Codex sign-in in this folder".into()))?;
    parse_auth(&text)
}

/// A JWT's `exp` claim, read without checking the signature (the token is
/// only ever sent to the server that issued it).
fn jwt_exp(token: &str) -> Option<i64> {
    let payload = token.split('.').nth(1)?;
    let v: Value = serde_json::from_slice(&base64url(payload)?).ok()?;
    v.get("exp")?.as_i64()
}

fn base64url(s: &str) -> Option<Vec<u8>> {
    let val = |c: u8| -> Option<u32> {
        Some(match c {
            b'A'..=b'Z' => c - b'A',
            b'a'..=b'z' => c - b'a' + 26,
            b'0'..=b'9' => c - b'0' + 52,
            b'-' | b'+' => 62,
            b'_' | b'/' => 63,
            _ => return None,
        } as u32)
    };
    let bytes: Vec<u8> = s.bytes().filter(|b| *b != b'=').collect();
    let mut out = Vec::with_capacity(bytes.len() * 3 / 4);
    for chunk in bytes.chunks(4) {
        let mut n = 0u32;
        for (i, c) in chunk.iter().enumerate() {
            n |= val(*c)? << (18 - 6 * i);
        }
        let take = match chunk.len() {
            4 => 3,
            3 => 2,
            2 => 1,
            _ => return None,
        };
        out.extend_from_slice(&n.to_be_bytes()[1..1 + take]);
    }
    Some(out)
}

fn epoch(t: i64) -> Option<DateTime<Utc>> {
    DateTime::<Utc>::from_timestamp(t, 0)
}

/// An ISO-8601 time or unix seconds.
fn time_of(v: &Value) -> Option<DateTime<Utc>> {
    match v {
        Value::String(s) => DateTime::parse_from_rfc3339(s).ok().map(|t| t.with_timezone(&Utc)),
        Value::Number(n) => n.as_i64().and_then(epoch),
        _ => None,
    }
}

fn pct(used: f64) -> u32 {
    used.round().max(0.0) as u32
}

/// The session and week of one endpoint `rate_limit` block, each slot
/// classified by its length.
fn endpoint_windows(rl: &Value, now: DateTime<Utc>) -> (Option<Window>, Option<Window>) {
    let (mut session, mut week) = (None, None);
    for slot in ["primary_window", "secondary_window"] {
        let Some(w) = rl.get(slot).filter(|w| w.is_object()) else { continue };
        let Some(kind) = w.get("limit_window_seconds").and_then(Value::as_i64).and_then(classify) else { continue };
        let Some(used) = w.get("used_percent").and_then(Value::as_f64) else { continue };
        let resets_at = w.get("reset_at").and_then(Value::as_i64).and_then(epoch).or_else(|| {
            w.get("reset_after_seconds").and_then(Value::as_i64).map(|s| now + chrono::Duration::seconds(s))
        });
        let win = Window { pct: pct(used), resets_at };
        match kind {
            WindowKind::Session => session = Some(win),
            WindowKind::Week => week = Some(win),
        }
    }
    (session, week)
}

/// A reading from its windows: weekly-only when there is no 5-hour window.
fn reading(session: Option<Window>, week: Option<Window>, resets: Option<ResetCredits>) -> UsageReading {
    UsageReading {
        session_limit: if session.is_some() { SessionLimit::Applies } else { SessionLimit::Absent },
        session,
        week,
        provider: Provider::Codex,
        resets,
        ..Default::default()
    }
}

/// Parse the usage endpoint's answer. Only the account's own `rate_limit`
/// is read; with no session or week there, it's a miss, never a zero.
pub fn parse_usage(body: &str, now: DateTime<Utc>) -> Result<UsageReading, UsageError> {
    let v: Value =
        serde_json::from_str(body.trim()).map_err(|e| UsageError::BadOutput(format!("Codex usage isn't JSON: {e}")))?;
    let (session, week) = v.get("rate_limit").map_or((None, None), |rl| endpoint_windows(rl, now));
    if session.is_none() && week.is_none() {
        return Err(UsageError::NoUsageLines("Codex usage has no account limit".into()));
    }
    let resets = v
        .get("rate_limit_reset_credits")
        .and_then(|c| c.get("available_count"))
        .and_then(Value::as_u64)
        .map(|n| ResetCredits { held: n.min(u32::MAX as u64) as u32, ..Default::default() });
    Ok(reading(session, week, resets))
}

/// An HTTP answer: status, `retry-after`, body.
pub type Answer = (u16, Option<String>, String);

/// Map an endpoint answer to a reading or a miss.
pub fn interpret(answer: Answer, now: DateTime<Utc>) -> Result<UsageReading, UsageError> {
    let (status, retry_after, body) = answer;
    match status {
        200..=299 => parse_usage(&body, now),
        401 | 403 => Err(UsageError::TokenRejected),
        429 => Err(UsageError::Throttled {
            retry_after_secs: retry_after.and_then(|r| r.trim().parse::<i64>().ok()).filter(|s| *s > 0),
        }),
        s => Err(UsageError::BadOutput(format!("Codex usage endpoint answered HTTP {s}"))),
    }
}

/// When each held credit expires, soonest first (`credits[]` whose `status`
/// is `available`). `None` when the answer isn't readable.
pub fn parse_credit_expiries(body: &str) -> Option<Vec<DateTime<Utc>>> {
    let v: Value = serde_json::from_str(body.trim()).ok()?;
    let mut out: Vec<DateTime<Utc>> = v
        .get("credits")?
        .as_array()?
        .iter()
        .filter(|c| c.get("status").and_then(Value::as_str) == Some("available"))
        .filter_map(|c| c.get("expires_at").and_then(time_of))
        .collect();
    out.sort();
    Some(out)
}

/// The newest account record in a session log's text that may stand in
/// for the endpoint at `now`: `limit_id` "codex" (never a model's bucket),
/// under [`LOG_MAX_AGE_SECS`] old, and with no window that has reset since
/// (its number would describe a window that's over). An older record never
/// stands in for a refused newer one, nor for anything at or before
/// `not_before` (the last endpoint reading, which it would only take back).
pub fn reading_from_log(text: &str, now: DateTime<Utc>, not_before: Option<DateTime<Utc>>) -> Option<UsageReading> {
    for line in text.lines().rev() {
        if !line.contains("\"rate_limits\"") {
            continue;
        }
        let Ok(v) = serde_json::from_str::<Value>(line) else { continue };
        let Some(rl) = v.get("payload").and_then(|p| p.get("rate_limits")).filter(|r| r.is_object()) else {
            continue;
        };
        if rl.get("limit_id").and_then(Value::as_str) != Some(ACCOUNT_LIMIT_ID) {
            continue;
        }
        let at = v.get("timestamp").and_then(time_of)?;
        if (now - at).num_seconds() > LOG_MAX_AGE_SECS
            || at > now + chrono::Duration::seconds(300)
            || not_before.is_some_and(|t| at <= t)
        {
            return None;
        }
        let (mut session, mut week) = (None, None);
        for slot in ["primary", "secondary"] {
            let Some(w) = rl.get(slot).filter(|w| w.is_object()) else { continue };
            let Some(kind) = w.get("window_minutes").and_then(Value::as_i64).and_then(|m| classify(m * 60)) else {
                continue;
            };
            let used = w.get("used_percent").and_then(Value::as_f64)?;
            let resets_at = w.get("resets_at").and_then(Value::as_i64).and_then(epoch);
            if resets_at.is_none_or(|t| t <= now) {
                return None;
            }
            let win = Window { pct: pct(used), resets_at };
            match kind {
                WindowKind::Session => session = Some(win),
                WindowKind::Week => week = Some(win),
            }
        }
        if session.is_none() && week.is_none() {
            return None;
        }
        return Some(reading(session, week, None));
    }
    None
}

/// The tail of a file: its newest records (the first, partial line of a
/// cut is skipped by the parser, which ignores lines that aren't JSON).
fn tail(path: &Path) -> Option<String> {
    let mut f = std::fs::File::open(path).ok()?;
    let len = f.metadata().ok()?.len();
    f.seek(SeekFrom::Start(len.saturating_sub(LOG_TAIL_BYTES))).ok()?;
    let mut buf = Vec::new();
    f.read_to_end(&mut buf).ok()?;
    Some(String::from_utf8_lossy(&buf).into_owned())
}

/// The session-log fallback for a Codex home: the newest rollout files
/// (`sessions/YYYY/MM/DD/*.jsonl`, the two newest days, written in the last
/// hour), newest first, through [`reading_from_log`].
pub fn reading_from_session_logs(
    dir: &Path,
    now: DateTime<Utc>,
    not_before: Option<DateTime<Utc>>,
) -> Option<UsageReading> {
    let subdirs = |p: &Path| -> Vec<PathBuf> {
        let mut v: Vec<PathBuf> = std::fs::read_dir(p)
            .map(|rd| rd.flatten().map(|e| e.path()).filter(|p| p.is_dir()).collect())
            .unwrap_or_default();
        v.sort();
        v.reverse();
        v
    };
    let mut days = Vec::new();
    'years: for y in subdirs(&dir.join("sessions")) {
        for m in subdirs(&y) {
            for d in subdirs(&m) {
                days.push(d);
                if days.len() == 2 {
                    break 'years;
                }
            }
        }
    }
    let cutoff = std::time::SystemTime::from(now) - Duration::from_secs(LOG_MAX_AGE_SECS as u64);
    let mut files: Vec<(std::time::SystemTime, PathBuf)> = days
        .iter()
        .flat_map(|d| std::fs::read_dir(d).into_iter().flatten().flatten())
        .map(|e| e.path())
        .filter(|p| p.extension().is_some_and(|x| x == "jsonl"))
        .filter_map(|p| Some((std::fs::metadata(&p).ok()?.modified().ok()?, p)))
        .filter(|(t, _)| *t >= cutoff)
        .collect();
    files.sort();
    let newest = files.into_iter().rev().find_map(|(_, p)| {
        let text = tail(&p)?;
        text.contains("\"limit_id\":\"codex\"").then_some(text)
    })?;
    reading_from_log(&newest, now, not_before)
}

/// One account's reset-credit details, kept between polls.
#[derive(Debug, Clone, PartialEq, Eq)]
struct CreditsEntry {
    fetched_at: i64,
    held: u32,
    expiries: Vec<DateTime<Utc>>,
}

/// What a Codex poll keeps per account between polls: the reset-credit
/// details, so the extra call happens hourly or when the held count
/// changes, and when the endpoint last answered, so the log never takes a
/// reading back.
#[derive(Debug, Clone, Default)]
pub struct CodexCache {
    entries: HashMap<PathBuf, CreditsEntry>,
    endpoint_ok: HashMap<PathBuf, DateTime<Utc>>,
}

impl CodexCache {
    /// The details for `held` credits, as last fetched (within the TTL).
    fn details(&self, dir: &Path, now: i64) -> Option<&CreditsEntry> {
        self.entries.get(dir).filter(|e| now - e.fetched_at < CREDITS_TTL_SECS)
    }

    /// The details to show with `held` credits: fetched again when the
    /// cache is older than the TTL or holds another count. A failed fetch
    /// keeps what was known for the same count, and otherwise knows nothing
    /// (dates that disagree with the count are worse than none).
    fn refresh(
        &mut self,
        dir: &Path,
        held: u32,
        now: i64,
        get: &mut dyn FnMut(&str) -> Option<String>,
    ) -> ResetCredits {
        let cached = self.details(dir, now).filter(|e| e.held == held).cloned();
        let entry = match cached {
            Some(e) => e,
            None => {
                let same = self.entries.get(dir).filter(|e| e.held == held);
                let expiries = get(CREDITS_URL)
                    .as_deref()
                    .and_then(parse_credit_expiries)
                    .or_else(|| same.map(|e| e.expiries.clone()))
                    .unwrap_or_default();
                let e = CreditsEntry { fetched_at: now, held, expiries };
                self.entries.insert(dir.to_path_buf(), e.clone());
                e
            }
        };
        ResetCredits { held, expiries: ahead(&entry.expiries, now) }
    }

    /// What the last good poll said, for a session-log reading (which has
    /// no credits) while it is within the TTL. Its count is the endpoint's
    /// when it last answered, so credits that expired since are taken off,
    /// as `ResetCredits::at` takes them off a kept reading.
    fn last_known(&self, dir: &Path, now: i64) -> Option<ResetCredits> {
        let e = self.details(dir, now)?;
        let since = self.endpoint_ok.get(dir).map_or(e.fetched_at, |t| t.timestamp());
        let gone = e.expiries.iter().filter(|t| t.timestamp() > since && t.timestamp() <= now).count() as u32;
        Some(ResetCredits { held: e.held.saturating_sub(gone), expiries: ahead(&e.expiries, now) })
    }
}

/// The expiries still ahead at `now`, soonest first, at most
/// [`ResetCredits::MAX_EXPIRIES`].
fn ahead(expiries: &[DateTime<Utc>], now: i64) -> Vec<DateTime<Utc>> {
    expiries.iter().copied().filter(|t| t.timestamp() > now).take(ResetCredits::MAX_EXPIRIES).collect()
}

/// One GET with the account's sign-in (blocking).
pub fn http_get(url: &str, auth: &Auth) -> Result<Answer, UsageError> {
    let agent: ureq::Agent =
        ureq::Agent::config_builder().http_status_as_error(false).timeout_global(Some(HTTP_TIMEOUT)).build().into();
    let mut req = agent
        .get(url)
        .header("Authorization", &format!("Bearer {}", auth.access))
        .header("User-Agent", &crate::oauth::user_agent())
        .header("Accept", "application/json");
    if let Some(id) = &auth.account_id {
        req = req.header("ChatGPT-Account-Id", id);
    }
    let mut resp = req.call().map_err(|e| match e {
        ureq::Error::Timeout(_) => UsageError::Timeout,
        e => UsageError::Offline(e.to_string()),
    })?;
    let status = resp.status().as_u16();
    let retry_after = resp.headers().get("retry-after").and_then(|v| v.to_str().ok()).map(str::to_owned);
    let body = resp.body_mut().read_to_string().unwrap_or_default();
    Ok((status, retry_after, body))
}

/// Read one Codex account. A miss that may pass (a timeout, the endpoint
/// unreachable, a 429, an odd answer) falls back to the session logs; a
/// sign-in problem doesn't (no data).
pub fn poll(dir: &Path, now: DateTime<Utc>, cache: &mut CodexCache) -> Polled {
    poll_with(dir, now, cache, &mut http_get, &mut |d, t, nb| reading_from_session_logs(d, t, nb))
}

/// The session-log fallback: (home, now, the last endpoint reading) → a
/// reading ([`reading_from_session_logs`]).
pub type LogSource<'a> = dyn FnMut(&Path, DateTime<Utc>, Option<DateTime<Utc>>) -> Option<UsageReading> + 'a;

/// [`poll`] with the network and the session logs injected (tests).
pub fn poll_with(
    dir: &Path,
    now: DateTime<Utc>,
    cache: &mut CodexCache,
    get: &mut dyn FnMut(&str, &Auth) -> Result<Answer, UsageError>,
    logs: &mut LogSource<'_>,
) -> Polled {
    let auth = match read_auth(dir) {
        Ok(a) => a,
        Err(e) => return miss(e),
    };
    if auth.expired(now.timestamp()) {
        return miss(UsageError::TokenExpired);
    }
    match get(USAGE_URL, &auth).and_then(|a| interpret(a, now)) {
        Ok(mut r) => {
            cache.endpoint_ok.insert(dir.to_path_buf(), now);
            if let Some(held) = r.resets.as_ref().map(|c| c.held) {
                let mut fetch =
                    |url: &str| get(url, &auth).ok().filter(|(s, _, _)| (200..300).contains(s)).map(|(_, _, b)| b);
                r.resets = Some(cache.refresh(dir, held, now.timestamp(), &mut fetch));
            }
            Polled::from(Ok::<_, UsageError>(r))
        }
        Err(e) if e.is_transient() => match logs(dir, now, cache.endpoint_ok.get(dir).copied()) {
            Some(mut r) => {
                r.resets = cache.last_known(dir, now.timestamp());
                Polled { result: Ok(r), idle: false, probe: ProbeRun::NotRun, idle_miss: Some(e) }
            }
            None => miss(e),
        },
        Err(e) => miss(e),
    }
}

fn miss(e: UsageError) -> Polled {
    Polled::from(Err::<UsageReading, _>(e))
}

/// When a Codex account is due: never polled, at once; then every
/// [`SHOWN_EVERY_SECS`] while shown and [`OTHER_EVERY_SECS`] otherwise, or
/// after its window resets or a refresh is asked for; never within
/// [`MIN_GAP_SECS`] of the last try, nor while it's backing off.
pub fn due_at(
    last_attempt: Option<i64>,
    shown: bool,
    wake_at: Option<i64>,
    requested_at: Option<i64>,
    blocked_until: Option<i64>,
) -> i64 {
    let Some(last) = last_attempt else { return blocked_until.unwrap_or(i64::MIN) };
    let every = if shown { SHOWN_EVERY_SECS } else { OTHER_EVERY_SECS };
    let mut due = last + every;
    for t in [wake_at, requested_at].into_iter().flatten().filter(|t| *t > last) {
        due = due.min(t);
    }
    due.max(last + MIN_GAP_SECS).max(blocked_until.unwrap_or(i64::MIN))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn at(s: &str) -> DateTime<Utc> {
        DateTime::parse_from_rfc3339(s).unwrap().with_timezone(&Utc)
    }

    fn fixture(name: &str) -> String {
        std::fs::read_to_string(Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/codex").join(name)).unwrap()
    }

    fn jwt(exp: i64) -> String {
        let enc = |s: &str| {
            const A: &[u8] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
            let b = s.as_bytes();
            let mut out = String::new();
            for c in b.chunks(3) {
                let n = (c[0] as u32) << 16 | (*c.get(1).unwrap_or(&0) as u32) << 8 | *c.get(2).unwrap_or(&0) as u32;
                for i in 0..=c.len() {
                    out.push(A[(n >> (18 - 6 * i) & 63) as usize] as char);
                }
            }
            out
        };
        format!("{}.{}.sig", enc(r#"{"alg":"RS256"}"#), enc(&format!(r#"{{"exp":{exp},"sub":"x"}}"#)))
    }

    const NOW: &str = "2026-11-16T02:00:00Z";

    // The live $100 Pro plan (scrubbed): a weekly window in `primary`, no
    // 5-hour window, two reset credits held.
    #[test]
    fn live_pro_plan_is_weekly_only() {
        let r = parse_usage(&fixture("usage-pro-weekly.json"), at(NOW)).unwrap();
        assert_eq!(r.provider, Provider::Codex);
        assert_eq!(r.session, None);
        assert_eq!(r.week, Some(Window { pct: 0, resets_at: epoch(1_795_303_084) }));
        assert_eq!(r.session_limit, SessionLimit::Absent);
        assert!(r.no_session_limit());
        assert_eq!(r.resets, Some(ResetCredits { held: 2, expiries: vec![] }));
    }

    // A plan with both windows, and a Spark bucket at 0 % beside it: the
    // account's own numbers win, whatever the model bucket says.
    #[test]
    fn account_limit_never_a_model_bucket() {
        let r = parse_usage(&fixture("usage-plus-5h.json"), at(NOW)).unwrap();
        assert_eq!(r.session.as_ref().map(|w| w.pct), Some(34));
        assert_eq!(r.week.as_ref().map(|w| w.pct), Some(62), "61.6 rounds");
        assert_eq!(r.session_limit, SessionLimit::Applies);
        assert_eq!(r.resets.map(|c| c.held), Some(3));
        // Only model buckets: a miss, never their 0 %.
        assert!(matches!(
            parse_usage(&fixture("usage-no-account-limit.json"), at(NOW)),
            Err(UsageError::NoUsageLines(_))
        ));
    }

    // primary / secondary are positions: classify by length.
    #[test]
    fn windows_by_duration_not_position() {
        let r = parse_usage(&fixture("usage-swapped-slots.json"), at(NOW)).unwrap();
        assert_eq!(r.session.map(|w| w.pct), Some(12));
        assert_eq!(r.week.map(|w| w.pct), Some(78));
        assert_eq!(classify(18_000), Some(WindowKind::Session));
        assert_eq!(classify(604_800), Some(WindowKind::Week));
        assert_eq!(classify(0), None);
        assert_eq!(classify(86_400), None, "a daily window isn't read as either");
        // A reset_after_seconds alone still gives the reset.
        let body = r#"{"rate_limit":{"primary_window":{"used_percent":5,"limit_window_seconds":604800,"reset_after_seconds":3600}}}"#;
        let r = parse_usage(body, at(NOW)).unwrap();
        assert_eq!(r.week.unwrap().resets_at, Some(at("2026-11-16T03:00:00Z")));
        assert_eq!(r.resets, None, "no credits block: unknown, not zero");
    }

    #[test]
    fn endpoint_statuses() {
        let now = at(NOW);
        assert_eq!(interpret((401, None, String::new()), now), Err(UsageError::TokenRejected));
        assert_eq!(interpret((403, None, String::new()), now), Err(UsageError::TokenRejected));
        assert_eq!(
            interpret((429, Some("120".into()), String::new()), now),
            Err(UsageError::Throttled { retry_after_secs: Some(120) })
        );
        assert!(matches!(interpret((502, None, String::new()), now), Err(UsageError::BadOutput(_))));
        assert!(matches!(interpret((200, None, "<html>".into()), now), Err(UsageError::BadOutput(_))));
    }

    #[test]
    fn auth_json() {
        let t = jwt(1_795_000_000);
        let a = parse_auth(&format!(
            r#"{{"auth_mode":"chatgpt","OPENAI_API_KEY":null,"tokens":{{"id_token":"i","access_token":"{t}","refresh_token":"r","account_id":"acct"}}}}"#
        ))
        .unwrap();
        assert_eq!((a.account_id.as_deref(), a.expires_at), (Some("acct"), Some(1_795_000_000)));
        assert!(!a.expired(1_794_000_000));
        assert!(a.expired(1_794_999_950), "a minute's slack");
        assert!(!format!("{a:?}").contains(&t) && !format!("{a:?}").contains("acct"), "never logged");
        // An API-key sign-in has no ChatGPT tokens: not a usage account.
        assert!(matches!(parse_auth(r#"{"OPENAI_API_KEY":"sk-x","tokens":null}"#), Err(UsageError::NotLoggedIn(_))));
        assert!(matches!(parse_auth("nope"), Err(UsageError::NotLoggedIn(_))));
        assert_eq!(base64url("eyJhIjoxfQ"), Some(br#"{"a":1}"#.to_vec()));
    }

    #[test]
    fn reset_credit_details() {
        let exp = parse_credit_expiries(&fixture("reset-credits.json")).unwrap();
        assert_eq!(exp, vec![at("2026-11-27T09:00:00Z"), at("2026-12-10T18:30:00Z")], "held ones, soonest first");
        assert_eq!(parse_credit_expiries("{}"), None);
    }

    // The log fallback: the newest account record, under an hour old and
    // with no window reset since; a newer model bucket never counts.
    #[test]
    fn stale_log_guard() {
        let log = fixture("rollout-weekly.jsonl");
        let r = reading_from_log(&log, at("2026-11-16T01:50:00Z"), None).unwrap();
        assert_eq!(r.week.as_ref().map(|w| w.pct), Some(34), "the account record, not Spark's 0 %");
        assert!(r.no_session_limit());
        assert_eq!(r.provider, Provider::Codex);
        // Over an hour old: refused.
        assert_eq!(reading_from_log(&log, at("2026-11-16T02:20:01Z"), None), None);
        // Its window has reset since (1795303084 = 2026-11-21T23:18:04Z).
        assert_eq!(reading_from_log(&log, at("2026-11-21T23:18:04Z"), None), None);
        let reset = log.replace("1795303084", "1794790000");
        assert_eq!(reading_from_log(&reset, at("2026-11-16T01:50:00Z"), None), None, "window already over");
        // Only model buckets: nothing.
        let spark = log.replace(r#""limit_id":"codex","#, r#""limit_id":"premium","#);
        assert_eq!(reading_from_log(&spark, at("2026-11-16T01:50:00Z"), None), None);
        // Older than the endpoint's last reading: it would take it back.
        let t = at("2026-11-16T01:50:00Z");
        assert_eq!(reading_from_log(&log, t, Some(at("2026-11-16T01:30:00Z"))), None);
        assert!(reading_from_log(&log, t, Some(at("2026-11-16T01:10:00Z"))).is_some());
    }

    #[test]
    fn session_logs_on_disk() {
        let root = std::env::temp_dir().join(format!("cm-codex-logs-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        let now = Utc::now();
        let day = root.join("sessions/2026/11/16");
        std::fs::create_dir_all(&day).unwrap();
        let ts = (now - chrono::Duration::minutes(5)).to_rfc3339_opts(chrono::SecondsFormat::Millis, true);
        let reset = now.timestamp() + 86_400;
        let rec = format!(
            r#"{{"timestamp":"{ts}","type":"event_msg","payload":{{"type":"token_count","rate_limits":{{"limit_id":"codex","primary":{{"used_percent":41.0,"window_minutes":10080,"resets_at":{reset}}},"secondary":null}}}}}}"#
        );
        std::fs::write(day.join("rollout-a.jsonl"), format!("{rec}\n")).unwrap();
        // An older day dir with garbage is never read past the newest two.
        std::fs::create_dir_all(root.join("sessions/2025/01/01")).unwrap();
        assert_eq!(reading_from_session_logs(&root, now, None).and_then(|r| r.week).map(|w| w.pct), Some(41));
        assert_eq!(reading_from_session_logs(&root.join("nope"), now, None), None);
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn folders() {
        let home = Path::new("/home/u");
        assert_eq!(default_dir(home, None), PathBuf::from("/home/u/.codex"));
        assert_eq!(default_dir(home, Some(" ")), PathBuf::from("/home/u/.codex"));
        assert_eq!(default_dir(home, Some("/srv/codex")), PathBuf::from("/srv/codex"));
        assert_eq!(default_dir(home, Some("~/work-codex")), PathBuf::from("/home/u/work-codex"));
        let root = std::env::temp_dir().join(format!("cm-codex-dir-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        std::fs::create_dir_all(root.join(".codex")).unwrap();
        std::fs::create_dir_all(root.join(".claude")).unwrap();
        std::fs::write(root.join(".claude/.credentials.json"), "{}").unwrap();
        assert!(!is_codex_dir(&root.join(".codex")), "no auth.json yet");
        assert_eq!(discover(&root, None), None, "nothing to discover: no Codex account");
        std::fs::write(root.join(".codex/auth.json"), "{}").unwrap();
        assert!(is_codex_dir(&root.join(".codex")));
        assert!(!is_codex_dir(&root.join(".claude")));
        assert_eq!(discover(&root, None), Some(root.join(".codex")));
        let _ = std::fs::remove_dir_all(&root);
    }

    fn home_with_token(tag: &str, exp: i64) -> PathBuf {
        let root = std::env::temp_dir().join(format!("cm-codex-poll-{tag}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        std::fs::create_dir_all(&root).unwrap();
        std::fs::write(
            root.join("auth.json"),
            format!(r#"{{"tokens":{{"access_token":"{}","account_id":"acct"}}}}"#, jwt(exp)),
        )
        .unwrap();
        root
    }

    // The poll: endpoint, credits (cached), the stale-safe fallback, and no
    // call at all with an expired token.
    #[test]
    fn poll_paths() {
        let now = at(NOW);
        let dir = home_with_token("paths", now.timestamp() + 86_400);
        let mut cache = CodexCache::default();
        let mut calls: Vec<String> = Vec::new();
        let usage = fixture("usage-pro-weekly.json");
        let mut ok = |url: &str, a: &Auth| -> Result<Answer, UsageError> {
            assert_eq!(a.account_id.as_deref(), Some("acct"));
            calls.push(url.rsplit('/').next().unwrap().to_owned());
            Ok(match url {
                USAGE_URL => (200, None, usage.clone()),
                CREDITS_URL => (200, None, fixture("reset-credits.json")),
                _ => panic!("unexpected call {url}"),
            })
        };
        let mut no_logs = |_: &Path, _: DateTime<Utc>, _: Option<DateTime<Utc>>| -> Option<UsageReading> { None };
        let p = poll_with(&dir, now, &mut cache, &mut ok, &mut no_logs);
        assert!(p.idle);
        let resets = p.result.unwrap().resets.unwrap();
        assert_eq!(
            resets,
            ResetCredits { held: 2, expiries: vec![at("2026-11-27T09:00:00Z"), at("2026-12-10T18:30:00Z")] }
        );
        // Within the hour, same count: only the usage call.
        let p = poll_with(&dir, now + chrono::Duration::minutes(5), &mut cache, &mut ok, &mut no_logs);
        assert_eq!(p.result.unwrap().resets.unwrap().expiries.len(), 2);
        drop(ok);
        assert_eq!(calls, ["usage", "rate-limit-reset-credits", "usage"]);

        // The endpoint times out: a fresh log reading stands in, the miss
        // rides along (it backs the endpoint off), credits as last known.
        // (The fixture's record is from 01:20; the endpoint last read at
        // 02:05 below would refuse it, so this cache starts from a reading
        // before it.)
        let log = fixture("rollout-weekly.jsonl");
        let t = at("2026-11-16T01:50:00Z");
        cache.endpoint_ok.insert(dir.clone(), at("2026-11-16T01:00:00Z"));
        let mut slow = |_: &str, _: &Auth| -> Result<Answer, UsageError> { Err(UsageError::Timeout) };
        let mut logs = |_: &Path, n: DateTime<Utc>, nb: Option<DateTime<Utc>>| reading_from_log(&log, n, nb);
        let p = poll_with(&dir, t, &mut cache, &mut slow, &mut logs);
        assert_eq!((p.idle, p.idle_miss.clone()), (false, Some(UsageError::Timeout)));
        let r = p.result.unwrap();
        assert_eq!((r.week.unwrap().pct, r.resets.map(|c| c.held)), (34, Some(2)));
        // ... but a stale log is no data, never its old number.
        let p = poll_with(&dir, at("2026-11-16T03:00:00Z"), &mut cache, &mut slow, &mut logs);
        assert_eq!(p.result, Err(UsageError::Timeout));
        // A refused token is no data: no fallback.
        let mut refused = |_: &str, _: &Auth| -> Result<Answer, UsageError> { Ok((401, None, String::new())) };
        assert_eq!(poll_with(&dir, t, &mut cache, &mut refused, &mut logs).result, Err(UsageError::TokenRejected));

        // An expired token is never sent.
        let old = home_with_token("expired", now.timestamp() - 10);
        let mut never = |_: &str, _: &Auth| -> Result<Answer, UsageError> { panic!("called with an expired token") };
        assert_eq!(poll_with(&old, now, &mut cache, &mut never, &mut no_logs).result, Err(UsageError::TokenExpired));
        // No sign-in: no call either.
        let empty = std::env::temp_dir().join(format!("cm-codex-poll-none-{}", std::process::id()));
        assert!(matches!(
            poll_with(&empty, now, &mut cache, &mut never, &mut no_logs).result,
            Err(UsageError::NotLoggedIn(_))
        ));
        let _ = std::fs::remove_dir_all(&dir);
        let _ = std::fs::remove_dir_all(&old);
    }

    // A changed count fetches the details again; a failed fetch for a new
    // count knows nothing rather than show the old dates.
    #[test]
    fn credits_cache_follows_the_count() {
        let mut cache = CodexCache::default();
        let dir = Path::new("/c");
        let mut ok = |url: &str| -> Option<String> {
            assert_eq!(url, CREDITS_URL);
            Some(fixture("reset-credits.json"))
        };
        let t = at(NOW).timestamp();
        assert_eq!(cache.refresh(dir, 2, t, &mut ok).expiries.len(), 2);
        let mut down = |_: &str| -> Option<String> { None };
        let c = cache.refresh(dir, 1, t + 60, &mut down);
        assert_eq!((c.held, c.next_expiry()), (1, None));
        // Back up within the TTL: the entry for this count is kept; past the
        // TTL it is fetched again.
        assert!(cache.refresh(dir, 1, t + 120, &mut ok).expiries.is_empty(), "within the TTL, cached");
        assert_eq!(cache.refresh(dir, 1, t + 60 + CREDITS_TTL_SECS, &mut ok).expiries.len(), 2);
    }

    #[test]
    fn a_log_reading_drops_credits_expired_since_the_endpoint_answered() {
        // 08:30: the endpoint answers, two credits (Nov 27 09:00, Dec 10).
        let t0 = at("2026-11-27T08:30:00Z");
        let dir = home_with_token("log-expired-credit", t0.timestamp() + 86_400);
        let mut cache = CodexCache::default();
        let usage = fixture("usage-pro-weekly.json");
        let mut ok = |url: &str, _: &Auth| -> Result<Answer, UsageError> {
            Ok(match url {
                USAGE_URL => (200, None, usage.clone()),
                CREDITS_URL => (200, None, fixture("reset-credits.json")),
                _ => panic!("unexpected call {url}"),
            })
        };
        let mut no_logs = |_: &Path, _: DateTime<Utc>, _: Option<DateTime<Utc>>| -> Option<UsageReading> { None };
        let r0 = poll_with(&dir, t0, &mut cache, &mut ok, &mut no_logs).result.unwrap();
        assert_eq!(r0.resets.as_ref().map(|c| (c.held, c.expiries.len())), Some((2, 2)));
        // 09:10: a 429, and a session-log reading stands in. The Nov 27 credit
        // expired after the endpoint's count, so it's gone from both.
        let t1 = at("2026-11-27T09:10:00Z");
        let mut throttled =
            |_: &str, _: &Auth| -> Result<Answer, UsageError> { Ok((429, Some("3600".into()), String::new())) };
        let mut logs = |_: &Path, _: DateTime<Utc>, _: Option<DateTime<Utc>>| -> Option<UsageReading> {
            Some(reading(None, Some(Window { pct: 40, resets_at: None }), None))
        };
        let r1 = poll_with(&dir, t1, &mut cache, &mut throttled, &mut logs).result.unwrap();
        assert_eq!(r1.resets, Some(ResetCredits { held: 1, expiries: vec![at("2026-12-10T18:30:00Z")] }));
        // And the device is sent one credit, not the endpoint's old two.
        let p = crate::Payload::from_reading_in(&r1, at("2026-11-27T09:15:00Z"), &Utc);
        assert_eq!((p.rc, p.rx), (Some(1), Some(vec!["Dec 10".to_owned()])));
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn cadence() {
        assert_eq!(due_at(None, false, None, None, None), i64::MIN, "never polled: at once");
        assert_eq!(due_at(Some(1000), true, None, None, None), 1300);
        assert_eq!(due_at(Some(1000), false, None, None, None), 1600);
        assert_eq!(due_at(Some(1000), false, Some(1050), None, None), 1120, "a reset wakes it, after the gap");
        assert_eq!(due_at(Some(1000), false, None, Some(1200), None), 1200, "a refresh");
        assert_eq!(due_at(Some(1000), false, None, Some(900), None), 1600, "an old refresh doesn't count");
        assert_eq!(due_at(Some(1000), true, None, None, Some(5000)), 5000, "backing off");
    }
}
