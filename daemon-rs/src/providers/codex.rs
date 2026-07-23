use super::{Credential, NormalizedUsage, SupportTier, UsageProvider};
use async_trait::async_trait;
use serde_json::Value;
use std::time::{SystemTime, UNIX_EPOCH};

// UNVERIFIED: this endpoint is undocumented — the Codex TUI itself polls it
// for its own rate-limit display (see openai/codex#10869), but its exact
// response shape hasn't been confirmed against a real account in this repo.
// The field names below are best-effort guesses at a shape parallel to
// Anthropic's rate-limit headers; `poll_usage` logs the raw JSON on first
// failure to parse so this can be corrected once tested against a real
// Codex CLI login. Do not trust `acct`/pct numbers from this adapter without
// that verification pass (see openspec task 4.2).
const USAGE_URL: &str = "https://chatgpt.com/backend-api/wham/usage";

pub struct CodexProvider;

fn auth_path() -> Option<std::path::PathBuf> {
    Some(dirs::home_dir()?.join(".codex").join("auth.json"))
}

fn read_token() -> Option<String> {
    let path = auth_path()?;
    let blob = std::fs::read_to_string(path).ok()?;
    let data: Value = serde_json::from_str(&blob).ok()?;
    // Observed field name in community reports; may need adjustment once
    // verified against a real auth.json.
    data.get("access_token")
        .or_else(|| data.get("accessToken"))
        .and_then(Value::as_str)
        .map(str::to_string)
}

#[async_trait]
impl UsageProvider for CodexProvider {
    fn id(&self) -> &'static str {
        "codex"
    }

    fn support_tier(&self) -> SupportTier {
        SupportTier::Stable
    }

    async fn find_credentials(&self) -> Vec<Credential> {
        match read_token() {
            Some(token) => vec![Credential { label: "codex".to_string(), token }],
            None => {
                eprintln!("codex: no usable token in ~/.codex/auth.json this cycle");
                Vec::new()
            }
        }
    }

    async fn poll_usage(&self, cred: &Credential) -> Option<NormalizedUsage> {
        let client = reqwest::Client::new();
        let resp = client
            .get(USAGE_URL)
            .header("Authorization", format!("Bearer {}", cred.token))
            .send()
            .await
            .ok()?;

        if !resp.status().is_success() {
            eprintln!("codex: API HTTP {}", resp.status());
            return None;
        }

        let body: Value = match resp.json().await {
            Ok(v) => v,
            Err(e) => {
                eprintln!("codex: failed to parse usage response as JSON: {e}");
                return None;
            }
        };

        // Best-effort extraction — see module doc comment. Falls back to
        // "no data" (rather than a wrong-but-confident payload) if the
        // expected shape isn't found, and logs the raw body once so the
        // real shape can be captured and this fixed.
        let five_hour = body.pointer("/five_hour").or_else(|| body.pointer("/rate_limits/five_hour"));
        let weekly = body.pointer("/weekly").or_else(|| body.pointer("/rate_limits/weekly"));

        let (Some(five_hour), Some(weekly)) = (five_hour, weekly) else {
            eprintln!(
                "codex: usage response didn't match the expected shape — raw body: {body}. \
                 This adapter's field mapping needs updating (see src/providers/codex.rs)."
            );
            return None;
        };

        let pct_of = |v: &Value| -> f32 {
            v.get("used_percent")
                .or_else(|| v.get("percent_used"))
                .and_then(Value::as_f64)
                .unwrap_or(0.0) as f32
        };
        let reset_minutes_of = |v: &Value| -> i32 {
            let now = SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs() as i64).unwrap_or(0);
            v.get("resets_at")
                .and_then(Value::as_i64)
                .map(|ts| ((ts - now) / 60).max(0) as i32)
                .unwrap_or(0)
        };

        Some(NormalizedUsage {
            s: pct_of(five_hour),
            sr: reset_minutes_of(five_hour),
            w: pct_of(weekly),
            wr: reset_minutes_of(weekly),
            st: "allowed".to_string(),
            c: None,
            acct: "pro".to_string(),
            tp: None,
            pd: None,
            rd: None,
            t: None,
            tf: None,
            ok: true,
        })
    }
}
