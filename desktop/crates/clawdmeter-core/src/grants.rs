//! Claude's limit-reset grants ("Reset for free"): resets of the weekly
//! limit Anthropic hands out now and then, each with an end date.
//!
//! They are read from the usage endpoint with `?cedar_ember=1&skip_spend=1`,
//! which answers only Claude Code's own User-Agent
//! (`claude-cli/<version> (external, cli)`); any other gets
//! `ineligible_reason: "surface"` and no grants. That one call therefore
//! sends Claude Code's User-Agent, with the installed `claude --version`: read-only, at most
//! once a day per account, and only with `claude_resets = on` (default off).
//! CodexBar sends the same User-Agent for the same read. Everything else keeps
//! [`crate::oauth::user_agent`]. Nothing here ever redeems a grant.
//!
//! Parsing follows CodexBar's rules (`ClaudeRateLimitResetCredits.swift`): only
//! an `eligible` answer counts; a grant counts while it isn't paused, has
//! started and hasn't ended; a grant with an unreadable field is dropped;
//! more than 50 resets or 200 grants is not believed. Grant ids are never
//! read (a redeem would use them). Anything else is unknown, never 0.

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::time::Duration;

use chrono::{DateTime, Utc};
use serde::{Deserialize, Serialize};
use serde_json::Value;

use crate::oauth;
use crate::usage::{ResetCredits, ResetKind, UsageError};

/// The usage endpoint with the grants block.
pub const GRANTS_URL: &str = "https://api.anthropic.com/api/oauth/usage?cedar_ember=1&skip_spend=1";
/// How often an account's grants are read.
pub const EVERY_SECS: i64 = 24 * 3600;
const MAX_RESETS: u32 = 50;
const MAX_GRANTS: usize = 200;

/// Claude Code's User-Agent for `version` (`2.1.330`).
pub fn cli_user_agent(version: &str) -> String {
    format!("claude-cli/{version} (external, cli)")
}

/// The version out of `claude --version` ("2.1.330 (Claude Code)"); `None`
/// for anything that isn't a dotted number.
pub fn parse_version(out: &str) -> Option<String> {
    let v = out.split_whitespace().next()?;
    (v.split('.').count() >= 2 && v.split('.').all(|p| !p.is_empty() && p.bytes().all(|b| b.is_ascii_digit())))
        .then(|| v.to_owned())
}

/// Run `claude --version` (blocking, 10 s at most).
pub fn claude_version(claude: &Path, home: &Path) -> Option<String> {
    let mut cmd = std::process::Command::new(claude);
    cmd.arg("--version");
    crate::usage::for_account(&mut cmd, &home.join(".claude"), home);
    let out = crate::usage::run_cli(cmd, Duration::from_secs(10)).ok()?;
    parse_version(&out)
}

/// What one answer says.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Answer {
    /// The grants held now (possibly none).
    Held(ResetCredits),
    /// `eligible: false`, with the reason: no count to show (not a 0).
    Ineligible(String),
    /// A shape this doesn't know: keep what was known.
    Unreadable,
}

/// What an answer body says at `now`.
pub fn parse_answer(body: &str, now: DateTime<Utc>) -> Answer {
    let Ok(v) = serde_json::from_str::<Value>(body) else { return Answer::Unreadable };
    let Some(ce) = v.get("cedar_ember") else { return Answer::Unreadable };
    match ce.get("eligible").and_then(Value::as_bool) {
        Some(true) => parse_grants(body, now).map_or(Answer::Unreadable, Answer::Held),
        Some(false) => Answer::Ineligible(
            ce.get("ineligible_reason").and_then(Value::as_str).unwrap_or("unknown").chars().take(40).collect(),
        ),
        None => Answer::Unreadable,
    }
}

/// The grants held at `now` out of an answer body. `None`: ineligible, or
/// a shape this doesn't know (unknown, never a 0).
pub fn parse_grants(body: &str, now: DateTime<Utc>) -> Option<ResetCredits> {
    let v: Value = serde_json::from_str(body).ok()?;
    let ce = v.get("cedar_ember")?;
    if ce.get("eligible").and_then(Value::as_bool) != Some(true) {
        return None;
    }
    let grants = ce.get("grants")?.as_array()?;
    if grants.len() > MAX_GRANTS {
        return None;
    }
    let mut readable = 0usize;
    let mut known_kinds = true;
    let time = |g: &Value, k: &str| {
        g.get(k)
            .and_then(Value::as_str)
            .and_then(|s| DateTime::parse_from_rfc3339(s).ok())
            .map(|t| t.with_timezone(&Utc))
    };
    let mut held = 0u32;
    let mut expiries = Vec::new();
    for g in grants {
        let (Some(left), Some(start), Some(end)) =
            (g.get("resets_left").and_then(Value::as_u64), time(g, "starts_at"), time(g, "ends_at"))
        else {
            continue;
        };
        let paused = match g.get("paused") {
            None | Some(Value::Null) => false,
            Some(Value::Bool(b)) => *b,
            Some(_) => continue,
        };
        readable += 1;
        if paused || start > now || end <= now || left == 0 {
            continue;
        }
        let left = u32::try_from(left).ok()?;
        held = held.checked_add(left)?;
        if held > MAX_RESETS {
            return None;
        }
        // No `clears`: what it resets is unknown, and then so are all kinds.
        let clears: Option<Vec<&str>> =
            g.get("clears").and_then(Value::as_array).map(|a| a.iter().filter_map(Value::as_str).collect());
        known_kinds &= clears.is_some();
        let kind = ResetKind::from_clears(&clears.unwrap_or_default());
        expiries.extend(std::iter::repeat_n((end, kind), left as usize));
    }
    // Grants none of which could be read: the shape changed, not "none held".
    if readable == 0 && !grants.is_empty() {
        return None;
    }
    expiries.sort_by_key(|(t, _)| *t);
    expiries.truncate(ResetCredits::MAX_EXPIRIES);
    let (expiries, kinds): (Vec<_>, Vec<_>) = expiries.into_iter().unzip();
    let kinds = if known_kinds { kinds } else { Vec::new() };
    Some(ResetCredits { held, expiries, kinds })
}

/// Read one account's grants (blocking). The token is only read, never
/// refreshed.
pub fn fetch(config_dir: &Path, home: &Path, version: &str, now: DateTime<Utc>) -> Result<Answer, UsageError> {
    let token = oauth::read_token(config_dir, home, now.timestamp()).ok_or(UsageError::TokenExpired)?;
    if token.expired(now.timestamp()) {
        return Err(UsageError::TokenExpired);
    }
    let (status, retry_after, body) = oauth::get(GRANTS_URL, &token, &cli_user_agent(version))?;
    match status {
        200..=299 => Ok(parse_answer(&body, now)),
        401 | 403 => Err(UsageError::TokenRejected),
        429 => Err(UsageError::Throttled {
            retry_after_secs: retry_after.and_then(|r| r.trim().parse::<i64>().ok()).filter(|s| *s > 0),
        }),
        s => Err(UsageError::BadOutput(format!("grants answered HTTP {s}"))),
    }
}

/// What was last read for one account, kept across restarts.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Entry {
    /// When it was asked (unix s), answered or not.
    pub asked_at: i64,
    /// Grants held, when the answer said.
    pub held: Option<u32>,
    /// Each held one's end (unix s), soonest first.
    #[serde(default)]
    pub expiries: Vec<i64>,
    /// What each resets ([`ResetKind::code`]), in step with `expiries`.
    #[serde(default)]
    pub kinds: String,
}

impl Entry {
    /// As reset credits, when known.
    pub fn credits(&self) -> Option<ResetCredits> {
        Some(ResetCredits {
            held: self.held?,
            expiries: self.expiries.iter().filter_map(|t| DateTime::from_timestamp(*t, 0)).collect(),
            kinds: self.kinds.chars().map(ResetKind::from_code).collect(),
        })
    }
}

/// Every account's last read (`<state>/claude-grants.json`).
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct GrantsCache {
    /// By config dir.
    pub accounts: BTreeMap<PathBuf, Entry>,
}

impl GrantsCache {
    /// Load; a missing or unreadable file is empty.
    pub fn load(path: &Path) -> Self {
        std::fs::read_to_string(path).ok().and_then(|t| serde_json::from_str(&t).ok()).unwrap_or_default()
    }

    /// Save (temp file + rename).
    pub fn save(&self, path: &Path) -> std::io::Result<()> {
        let tmp = path.with_extension("json.tmp");
        std::fs::write(&tmp, serde_json::to_string_pretty(self).unwrap_or_default())?;
        std::fs::rename(tmp, path)
    }

    /// `dir` hasn't been asked in [`EVERY_SECS`]. Nothing else asks
    /// sooner: an answer without kinds (a grant with no `clears`) waits its
    /// day like any other.
    pub fn due(&self, dir: &Path, now: i64) -> bool {
        self.accounts.get(dir).is_none_or(|e| now - e.asked_at >= EVERY_SECS)
    }

    /// Note an answer for `dir` at `now`: the grants held, or `None` for
    /// no count (ineligible).
    pub fn record(&mut self, dir: &Path, now: i64, credits: Option<&ResetCredits>) {
        let entry = Entry {
            asked_at: now,
            held: credits.map(|c| c.held),
            expiries: credits.map(|c| c.expiries.iter().map(DateTime::timestamp).collect()).unwrap_or_default(),
            kinds: credits.map(|c| c.kinds.iter().map(|k| k.code()).collect()).unwrap_or_default(),
        };
        self.accounts.insert(dir.to_path_buf(), entry);
    }

    /// Note an ask that told nothing new (a failure, an unreadable answer):
    /// what was known stays, and the next ask waits a day.
    pub fn touch(&mut self, dir: &Path, now: i64) {
        let e = self.accounts.entry(dir.to_path_buf()).or_insert(Entry {
            asked_at: now,
            held: None,
            expiries: vec![],
            kinds: String::new(),
        });
        e.asked_at = now;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn at(s: &str) -> DateTime<Utc> {
        DateTime::parse_from_rfc3339(s).unwrap().with_timezone(&Utc)
    }

    /// The real answer (2026-11-20), one grant's count raised to 1 to see it
    /// counted, and two more grants: one not started, one paused.
    const ANSWER: &str = r#"{"five_hour":null,"cedar_ember":{"eligible":true,"ineligible_reason":null,"at_limit":false,"exhausted":[],
      "grants":[
        {"id":"full-reset-2611","label":"One full limit reset (November 2026)","resets_total":1,"resets_left":1,
         "starts_at":"2026-11-10T21:30:00+00:00","ends_at":"2026-12-10T22:00:00+00:00","clears":["seven_day"],"paused":false,"type":"weekly"},
        {"id":"launch-2610-fable","resets_total":1,"resets_left":0,"starts_at":"2026-11-09T18:00:00+00:00",
         "ends_at":"2027-03-09T07:59:00+00:00","clears":["seven_day_overage_included"],"type":"full"},
        {"id":"later","resets_left":2,"starts_at":"2026-12-01T00:00:00+00:00","ends_at":"2027-01-01T00:00:00+00:00"},
        {"id":"paused","resets_left":1,"paused":true,"starts_at":"2026-11-01T00:00:00+00:00","ends_at":"2027-01-01T00:00:00+00:00"},
        {"id":"broken","resets_left":"1","starts_at":"2026-11-01T00:00:00+00:00","ends_at":"2027-01-01T00:00:00+00:00"}],
      "weekly_resets_at":"2026-11-25T23:00:00+00:00"}}"#;

    #[test]
    fn reads_the_grants_held() {
        let now = at("2026-11-21T12:00:00Z");
        let held = |held, ends: &[&str], k: ResetKind| {
            Some(ResetCredits { held, expiries: ends.iter().map(|s| at(s)).collect(), kinds: vec![k; ends.len()] })
        };
        let ineligible = r#"{"cedar_ember":{"eligible":false,"ineligible_reason":"surface","grants":[]}}"#;
        let many = r#"{"cedar_ember":{"eligible":true,"grants":[{"resets_left":51,"starts_at":"2026-11-01T00:00:00Z","ends_at":"2027-01-01T00:00:00Z"}]}}"#;
        // (body, what it says)
        let cases: Vec<(&str, Option<ResetCredits>)> = vec![
            (ANSWER, held(1, &["2026-12-10T22:00:00Z"], ResetKind::Weekly)),
            (r#"{"cedar_ember":{"eligible":true,"grants":[]}}"#, held(0, &[], ResetKind::Weekly)),
            (ineligible, None),
            (r#"{"seven_day":{"utilization":3}}"#, None),
            (r#"{"cedar_ember":{"eligible":true}}"#, None),
            (many, None),
            ("not json", None),
        ];
        for (body, want) in cases {
            assert_eq!(parse_grants(body, now), want, "{body}");
        }
        // Grants none of which can be read: unknown, not "none held".
        let renamed = r#"{"cedar_ember":{"eligible":true,"grants":[{"left":1,"starts_at":"2026-11-01T00:00:00Z","ends_at":"2027-01-01T00:00:00Z"}]}}"#;
        assert_eq!(parse_grants(renamed, now), None);
        assert_eq!(parse_answer(renamed, now), Answer::Unreadable);
        assert_eq!(parse_answer(ineligible, now), Answer::Ineligible("surface".into()));
        assert_eq!(
            parse_answer(ANSWER, now),
            Answer::Held(held(1, &["2026-12-10T22:00:00Z"], ResetKind::Weekly).unwrap())
        );
        // What each resets, from its `clears`.
        let kinds = |body: &str| parse_grants(body, at("2026-12-11T00:00:00Z")).map(|c| c.kinds);
        let mixed = ANSWER.replace("\"resets_left\":0", "\"resets_left\":1");
        assert_eq!(parse_grants(&mixed, now).map(|c| c.kinds), Some(vec![ResetKind::Weekly, ResetKind::Fable]));
        assert_eq!(kinds(&mixed), Some(vec![]), "a grant without clears: kinds unknown");
        // After the end date the grant no longer counts.
        let later = parse_grants(ANSWER, at("2026-12-11T00:00:00Z")).unwrap();
        assert_eq!((later.held, later.expiries.len(), later.kinds.len()), (2, 2, 0), "no clears: kinds unknown");
        assert_eq!(cli_user_agent("2.1.330"), "claude-cli/2.1.330 (external, cli)");
        assert_eq!(parse_version("2.1.330 (Claude Code)\n").as_deref(), Some("2.1.330"));
        assert_eq!(parse_version("error: no"), None);
    }

    #[test]
    fn asks_once_a_day_and_keeps_what_it_knew() {
        let dir = Path::new("/h/.claude");
        let mut c = GrantsCache::default();
        assert!(c.due(dir, 1_000));
        let one = ResetCredits { held: 1, expiries: vec![at("2026-12-10T22:00:00Z")], kinds: vec![ResetKind::Fable] };
        c.record(dir, 1_000, Some(&one));
        assert!(!c.due(dir, 1_000 + EVERY_SECS - 1) && c.due(dir, 1_000 + EVERY_SECS));
        // Kinds unknown (a grant with no `clears`): still once a day.
        let mut unknown = c.clone();
        unknown.accounts.get_mut(dir).unwrap().kinds.clear();
        assert!(!unknown.due(dir, 1_001));
        // A failed ask waits a day too, and keeps the last answer.
        c.touch(dir, 1_000 + EVERY_SECS);
        assert!(!c.due(dir, 1_000 + EVERY_SECS + 60));
        assert_eq!(c.accounts[dir].credits(), Some(one));
        // Ineligible: no count any more.
        let mut gone = c.clone();
        gone.record(dir, 1_000 + 2 * EVERY_SECS, None);
        assert_eq!(gone.accounts[dir].credits(), None);
        let back: GrantsCache = serde_json::from_str(&serde_json::to_string(&c).unwrap()).unwrap();
        assert_eq!(back, c);
    }
}
