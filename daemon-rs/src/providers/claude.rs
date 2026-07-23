use super::{Credential, NormalizedUsage, SupportTier, UsageProvider};
use async_trait::async_trait;
use serde_json::Value;
use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

const API_URL: &str = "https://api.anthropic.com/v1/messages";
const KEYCHAIN_SERVICE: &str = "Claude Code-credentials";

pub struct ClaudeProvider {
    pub config_dirs: Vec<PathBuf>,
}

fn default_config_dir() -> PathBuf {
    dirs::home_dir().unwrap_or_default().join(".claude")
}

/// Pull `accessToken` out of a credentials JSON blob. Claude Code's file may
/// have the token directly (`{"accessToken": "..."}`) or nested under a key
/// like `claudeAiOauth` — but NOT under `mcpOAuth`, whose per-server entries
/// are one level deeper and never have `accessToken` as a direct child, so a
/// plain "does this value have an accessToken key" scan safely skips it.
/// Also returns the token's `expiresAt` (ms since epoch) if present, so
/// callers can reject an expired file token instead of trying it and only
/// then discovering the API rejects it (the stale-credentials bug class this
/// project already hit once in production).
fn extract_token(blob: &str) -> Option<(String, Option<i64>)> {
    let data: Value = serde_json::from_str(blob).ok()?;
    if let Some(tok) = data.get("accessToken").and_then(Value::as_str) {
        return Some((tok.to_string(), data.get("expiresAt").and_then(Value::as_i64)));
    }
    if let Value::Object(map) = &data {
        for v in map.values() {
            if let Some(tok) = v.get("accessToken").and_then(Value::as_str) {
                return Some((tok.to_string(), v.get("expiresAt").and_then(Value::as_i64)));
            }
        }
    }
    None
}

fn is_expired(expires_at_ms: Option<i64>) -> bool {
    let Some(expires_at_ms) = expires_at_ms else {
        return false; // no expiry info — assume usable, matches current Python behavior
    };
    let now_ms = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_millis() as i64)
        .unwrap_or(i64::MAX);
    now_ms >= expires_at_ms
}

fn read_file_token(dir: &Path) -> Option<(String, Option<i64>)> {
    let path = dir.join(".credentials.json");
    let blob = std::fs::read_to_string(path).ok()?;
    extract_token(&blob)
}

#[cfg(target_os = "macos")]
fn read_keychain_token() -> Option<String> {
    let user = std::env::var("USER").ok()?;
    let out = std::process::Command::new("security")
        .args(["find-generic-password", "-s", KEYCHAIN_SERVICE, "-a", &user, "-w"])
        .output()
        .ok()?;
    if !out.status.success() {
        return None;
    }
    let blob = String::from_utf8(out.stdout).ok()?;
    extract_token(&blob).map(|(tok, _)| tok)
}

#[cfg(not(target_os = "macos"))]
fn read_keychain_token() -> Option<String> {
    None
}

#[async_trait]
impl UsageProvider for ClaudeProvider {
    fn id(&self) -> &'static str {
        "claude"
    }

    fn support_tier(&self) -> SupportTier {
        SupportTier::Stable
    }

    async fn find_credentials(&self) -> Vec<Credential> {
        let mut creds = Vec::new();
        let default_dir = default_config_dir();

        for dir in &self.config_dirs {
            let dir = dir.clone();
            let label = dir.display().to_string();
            let file_result = read_file_token(&dir);

            let usable_file_token = match &file_result {
                Some((tok, expires_at)) if !is_expired(*expires_at) => Some(tok.clone()),
                Some((_, expires_at)) if is_expired(*expires_at) => {
                    eprintln!(
                        "claude[{label}]: credentials file token is expired \
                         (expiresAt in the past) — ignoring it rather than \
                         sending a doomed API call"
                    );
                    None
                }
                _ => None,
            };

            if let Some(tok) = usable_file_token {
                creds.push(Credential { label, token: tok });
                continue;
            }

            // No usable file token. Only the default dir falls back to
            // Keychain — matches current single-plan macOS behavior.
            if dir == default_dir {
                if let Some(tok) = read_keychain_token() {
                    creds.push(Credential { label, token: tok });
                    continue;
                }
            }

            eprintln!("claude[{label}]: no usable credential this cycle");
        }

        creds
    }

    async fn poll_usage(&self, cred: &Credential) -> Option<NormalizedUsage> {
        let client = reqwest::Client::new();
        let resp = client
            .post(API_URL)
            .header("Authorization", format!("Bearer {}", cred.token))
            .header("anthropic-version", "2023-06-01")
            .header("anthropic-beta", "oauth-2025-04-20")
            .json(&serde_json::json!({
                "model": "claude-haiku-4-5-20251001",
                "max_tokens": 1,
                "messages": [{"role": "user", "content": "hi"}],
            }))
            .send()
            .await
            .ok()?;

        if !resp.status().is_success() {
            eprintln!("claude[{}]: API HTTP {}", cred.label, resp.status());
            return None;
        }

        let headers = resp.headers().clone();
        let hdr = |name: &str| -> String {
            headers
                .get(name)
                .and_then(|v| v.to_str().ok())
                .unwrap_or("0")
                .to_string()
        };

        let now = SystemTime::now().duration_since(UNIX_EPOCH).ok()?.as_secs_f64();
        let reset_minutes = |reset_ts: &str| -> i32 {
            reset_ts
                .parse::<f64>()
                .ok()
                .map(|r| ((r - now) / 60.0).round().max(0.0) as i32)
                .unwrap_or(0)
        };
        let pct = |util: &str| -> f32 {
            util.parse::<f32>().map(|u| (u * 100.0).round()).unwrap_or(0.0)
        };

        // Pro/Max accounts expose 5h/7d windows; Enterprise/overage accounts
        // don't send that header at all and use a single spending-limit
        // model instead — ported from daemon/claude_usage_daemon.py's
        // poll_api, which branches on presence of the 5h-utilization header.
        if headers.get("anthropic-ratelimit-unified-5h-utilization").is_some() {
            Some(NormalizedUsage {
                s: pct(&hdr("anthropic-ratelimit-unified-5h-utilization")),
                sr: reset_minutes(&hdr("anthropic-ratelimit-unified-5h-reset")),
                w: pct(&hdr("anthropic-ratelimit-unified-7d-utilization")),
                wr: reset_minutes(&hdr("anthropic-ratelimit-unified-7d-reset")),
                st: hdr("anthropic-ratelimit-unified-5h-status"),
                c: None,
                acct: "pro".to_string(),
                tp: None,
                pd: None,
                rd: None,
                t: None,
                tf: None,
                ok: true,
            })
        } else {
            let reset_ts = hdr("anthropic-ratelimit-unified-overage-reset");
            let (tp, pd, rd) = billing_period_info(now, &reset_ts);
            Some(NormalizedUsage {
                s: pct(&hdr("anthropic-ratelimit-unified-overage-utilization")),
                sr: reset_minutes(&reset_ts),
                w: 0.0,
                wr: 0,
                st: hdr("anthropic-ratelimit-unified-status"),
                c: None,
                acct: "ent".to_string(),
                tp: Some(tp),
                pd: Some(pd),
                rd: Some(rd),
                t: None,
                tf: None,
                ok: true,
            })
        }
    }
}

/// Fraction of billing period elapsed (0-100), period length in days, and a
/// formatted reset date, assuming a calendar-monthly period ending at
/// `reset_ts` — ported from `_billing_period_info` in
/// `daemon/claude_usage_daemon.py`. The rate-limit headers only expose the
/// reset timestamp, not the period length, so "one calendar month back" is
/// an assumption matching that function's own documented caveat.
fn billing_period_info(now: f64, reset_ts: &str) -> (i32, i32, String) {
    use chrono::{Datelike, TimeZone, Timelike};

    let Ok(period_end) = reset_ts.parse::<f64>() else {
        return (0, 30, String::new());
    };
    let Some(dt_end) = chrono::Local.timestamp_opt(period_end as i64, 0).single() else {
        return (0, 30, String::new());
    };

    let (prev_year, prev_month) = if dt_end.month() > 1 {
        (dt_end.year(), dt_end.month() - 1)
    } else {
        (dt_end.year() - 1, 12)
    };
    let days_in_prev_month = days_in_month(prev_year, prev_month);
    let prev_day = dt_end.day().min(days_in_prev_month);
    let Some(dt_start) = chrono::Local
        .with_ymd_and_hms(prev_year, prev_month, prev_day, dt_end.hour(), dt_end.minute(), dt_end.second())
        .single()
    else {
        return (0, 30, String::new());
    };

    let period_start = dt_start.timestamp() as f64;
    let period_len = period_end - period_start;
    if period_len <= 0.0 {
        return (0, 30, String::new());
    }
    let pct_val = ((now - period_start) / period_len * 100.0).round().clamp(0.0, 100.0) as i32;
    let total_days = (period_len / 86400.0).round() as i32;
    let rd = dt_end.format("%b %-d").to_string();
    (pct_val, total_days, rd)
}

fn days_in_month(year: i32, month: u32) -> u32 {
    use chrono::{Datelike, NaiveDate};
    let (next_year, next_month) = if month == 12 { (year + 1, 1) } else { (year, month + 1) };
    NaiveDate::from_ymd_opt(next_year, next_month, 1)
        .unwrap()
        .pred_opt()
        .unwrap()
        .day()
}

#[cfg(test)]
mod tests {
    use super::*;

    // Reproduces the stale-credentials-file bug class this project hit in
    // production: a `.credentials.json` file with an `expiresAt` in the past
    // must not be treated as usable, even though it parses fine and looks
    // identical in shape to a valid token. See design.md's project memory /
    // the session that found `~/.claude/.credentials.json` (dated 2026-06-11,
    // expired ~2026-06-12) silently shadowing a valid, fresh Keychain token.

    #[test]
    fn expired_direct_token_is_detected() {
        let blob = r#"{"accessToken": "sk-ant-stale", "expiresAt": 1000}"#; // way in the past
        let (token, expires_at) = extract_token(blob).expect("should parse");
        assert_eq!(token, "sk-ant-stale");
        assert!(is_expired(expires_at), "a token with expiresAt=1000ms should be expired");
    }

    #[test]
    fn fresh_direct_token_is_not_expired() {
        let far_future_ms = 4_000_000_000_000i64; // year ~2096
        let blob = format!(r#"{{"accessToken": "sk-ant-fresh", "expiresAt": {far_future_ms}}}"#);
        let (_, expires_at) = extract_token(&blob).expect("should parse");
        assert!(!is_expired(expires_at));
    }

    #[test]
    fn nested_claude_ai_oauth_token_is_extracted_not_mcp_oauth() {
        // Mirrors the real credentials.json shape: top-level keys are
        // `mcpOAuth` (per-server blobs, one level too deep to have their own
        // `accessToken`) and `claudeAiOauth` (which does). The extractor must
        // find the latter, not be confused by the former.
        let blob = r#"{
            "mcpOAuth": {"some-server|abcd": {"accessToken": "should-not-be-picked"}},
            "claudeAiOauth": {"accessToken": "sk-ant-real", "expiresAt": 4000000000000}
        }"#;
        // NOTE: this fixture intentionally matches production shape, where
        // mcpOAuth's per-server entries DO carry their own accessToken one
        // level deeper than claudeAiOauth's. extract_token only checks
        // direct children of the top-level object, so mcpOAuth's value
        // (`{"some-server|abcd": {...}}`) has no direct "accessToken" key
        // and is correctly skipped in favor of claudeAiOauth.
        let (token, _) = extract_token(blob).expect("should parse");
        assert_eq!(token, "sk-ant-real");
    }

    #[test]
    fn no_expiry_field_is_treated_as_not_expired() {
        // Matches current Python daemon behavior: absence of expiresAt means
        // "assume usable" rather than "assume expired".
        assert!(!is_expired(None));
    }
}
