pub mod claude;
pub mod codex;

use async_trait::async_trait;
use serde::Serialize;

/// Where a provider's usage-checking path stands. Only `Stable` ships in this
/// change; `Experimental` is reserved for a future provider (e.g. Kiro) whose
/// non-interactive usage path isn't validated yet.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SupportTier {
    Stable,
}

/// Wire payload sent to the ESP32 over BLE. Field names and meanings match
/// `firmware/src/data.h`'s `UsageData` struct — see that file for semantics.
#[derive(Debug, Clone, Serialize)]
pub struct NormalizedUsage {
    pub s: f32,             // session_pct: 0-100
    pub sr: i32,            // session_reset_mins
    pub w: f32,             // weekly_pct: 0-100 (0 for Enterprise)
    pub wr: i32,            // weekly_reset_mins
    pub st: String,         // status: "allowed", "limited", etc.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub c: Option<bool>,    // chime opt-in
    pub acct: String,       // "pro" | "ent"
    #[serde(skip_serializing_if = "Option::is_none")]
    pub tp: Option<i32>,    // time_pct (Enterprise)
    #[serde(skip_serializing_if = "Option::is_none")]
    pub pd: Option<i32>,    // period_days (Enterprise)
    #[serde(skip_serializing_if = "Option::is_none")]
    pub rd: Option<String>, // reset_date (Enterprise)
    #[serde(skip_serializing_if = "Option::is_none")]
    pub t: Option<i64>,     // clock_epoch
    #[serde(skip_serializing_if = "Option::is_none")]
    pub tf: Option<i32>,    // clock_fmt: 12 | 24
    pub ok: bool,
}

/// Opaque credential handle a provider's `find_credentials` produces and its
/// own `poll_usage` consumes. Providers own their credential shape entirely —
/// shared code never inspects `token`, only uses `label` for logging/active-
/// selection bookkeeping (e.g. which config dir a candidate came from).
pub struct Credential {
    pub label: String,
    pub token: String,
}

/// A single AI tool's local usage-checking adapter. Shared daemon logic
/// (the poll loop, active-provider selection, BLE payload assembly) depends
/// only on this trait and must never branch on a specific provider's identity
/// — see openspec/changes/multi-provider-rust-daemon/design.md Decision 1.
///
/// `find_credentials` returns a Vec, not a single credential, because a
/// provider can have more than one independently-polled source — e.g.
/// Claude's `config_dirs` option polls multiple config directories, each
/// producing its own candidate for active-provider selection.
#[async_trait]
pub trait UsageProvider: Send + Sync {
    /// Stable identifier used in the `providers` config list (e.g. "claude").
    fn id(&self) -> &'static str;

    fn support_tier(&self) -> SupportTier;

    /// Look up this provider's local credential(s). An empty Vec means "not
    /// usable this cycle" (missing/unreadable), not an error to propagate.
    async fn find_credentials(&self) -> Vec<Credential>;

    /// One usage check against the provider's API for one credential. `None`
    /// means "no usable data this cycle" — network/parse errors are logged
    /// by the implementation and degrade to this, never panic or bubble up.
    async fn poll_usage(&self, cred: &Credential) -> Option<NormalizedUsage>;
}

/// The identifiers this build recognizes at all, supported or not. Used to
/// distinguish "not enabled" from "doesn't exist" when validating the
/// `providers` config list.
pub const KNOWN_PROVIDER_IDS: &[&str] = &["claude", "codex"];

pub const UNSUPPORTED_PROVIDER_IDS: &[&str] = &["cursor", "kiro", "gemini", "antigravity"];

/// Build the registry of providers named in the config's `providers` list.
/// Returns an error naming the first unrecognized identifier rather than
/// silently skipping it — see the provider-adapter spec's "Unsupported
/// providers are documented, not silently ignored" requirement.
pub fn build_registry(cfg: &crate::config::Config) -> Result<Vec<Box<dyn UsageProvider>>, String> {
    let mut registry: Vec<Box<dyn UsageProvider>> = Vec::new();
    for id in cfg.providers() {
        match id.as_str() {
            "claude" => registry.push(Box::new(claude::ClaudeProvider {
                config_dirs: cfg.claude_config_dirs(),
            })),
            "codex" => registry.push(Box::new(codex::CodexProvider)),
            other if UNSUPPORTED_PROVIDER_IDS.contains(&other) => {
                return Err(format!(
                    "provider '{other}' is not supported yet — see README's \"Other providers\" section for why"
                ));
            }
            other => {
                return Err(format!(
                    "unknown provider '{other}' in config — expected one of {KNOWN_PROVIDER_IDS:?}"
                ));
            }
        }
    }
    Ok(registry)
}
