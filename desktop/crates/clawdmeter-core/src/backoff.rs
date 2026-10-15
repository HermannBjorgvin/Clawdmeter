//! Per-account backoff after a failed `/usage` poll.
//!
//! `claude -p /usage` fetches `GET /api/oauth/usage`. When that endpoint
//! answers 429, Claude Code prints `/usage` without its `Current session` /
//! `Current week` lines and remembers the 429 per bearer token for about ten
//! minutes. Its debug log (`<config dir>/debug/latest`) shows it:
//!
//! ```text
//! [DEBUG] fetchUtilization: 429 remembered for this bearer; not asking again for 589s
//! [ERROR] Failed to load usage data: Request failed with status code 429
//! ```
//!
//! Polling inside that window gets nothing back, and polling right after it
//! can trip the limit again, so a miss that may have reached the endpoint
//! parks the account for [`RATE_LIMIT_FLOOR_SECS`], then longer on repeated
//! misses ([`BACKOFF_STEPS`]), until a poll succeeds. Nothing bypasses it:
//! device refresh requests and the tray's "Refresh now" wait too.

/// Claude Code's own 429 memory per bearer (about 600 s), so the shortest
/// pause after a miss.
pub const RATE_LIMIT_FLOOR_SECS: i64 = 600;

/// Pause after the 1st, 2nd and 3rd-or-later consecutive miss (s).
pub const BACKOFF_STEPS: [i64; 3] = [RATE_LIMIT_FLOOR_SECS, 1200, 1800];

/// Added to a retry-after read from Claude Code's debug log, so the next
/// poll lands after its 429 memory has expired.
pub const RETRY_AFTER_MARGIN_SECS: i64 = 10;

/// Backoff state for one account. Times are unix seconds.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct Backoff {
    strikes: u32,
    until: Option<i64>,
}

impl Backoff {
    /// A miss at `now`. `retry_after` is how long Claude Code said it won't
    /// ask again, when its debug log said so. Returns the pause in seconds.
    pub fn on_miss(&mut self, now: i64, retry_after: Option<i64>) -> i64 {
        let step = BACKOFF_STEPS[(self.strikes as usize).min(BACKOFF_STEPS.len() - 1)];
        let hinted = retry_after.map_or(0, |s| s.max(0).saturating_add(RETRY_AFTER_MARGIN_SECS));
        let pause = step.max(hinted);
        self.strikes = self.strikes.saturating_add(1);
        self.until = Some(now.saturating_add(pause));
        pause
    }

    /// The server said when to come back (a 429's `retry-after`): park for
    /// exactly that long. Counts as a strike for later misses.
    pub fn park_exact(&mut self, now: i64, secs: i64) {
        self.strikes = self.strikes.saturating_add(1);
        self.until = Some(now.saturating_add(secs.max(0)));
    }

    /// Restore a saved block.
    pub fn restore(strikes: u32, until: i64) -> Self {
        Backoff { strikes, until: Some(until) }
    }

    /// A good poll: back to normal.
    pub fn on_success(&mut self) {
        *self = Backoff::default();
    }

    /// Unix seconds before which the account must not be polled.
    pub fn until(&self) -> Option<i64> {
        self.until
    }

    /// The account is parked at `now`.
    pub fn active(&self, now: i64) -> bool {
        self.until.is_some_and(|u| u > now)
    }

    /// Seconds left at `now`, while parked.
    pub fn remaining(&self, now: i64) -> Option<i64> {
        self.until.map(|u| u - now).filter(|r| *r > 0)
    }

    /// Consecutive misses that backed off.
    pub fn strikes(&self) -> u32 {
        self.strikes
    }
}

/// One account's saved block.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct SavedBlock {
    /// Strikes so far.
    pub strikes: u32,
    /// Unix seconds before which the account's usage endpoint is left alone.
    pub until: i64,
}

/// Read the saved blocks (`{"<config dir>": {"until": …, "strikes": …}}`);
/// a missing or unreadable file is no blocks.
pub fn load_blocks(path: &std::path::Path) -> std::collections::BTreeMap<std::path::PathBuf, SavedBlock> {
    let Ok(text) = std::fs::read_to_string(path) else { return Default::default() };
    let Ok(serde_json::Value::Object(m)) = serde_json::from_str(&text) else { return Default::default() };
    m.into_iter()
        .filter_map(|(dir, v)| {
            let until = v.get("until")?.as_i64()?;
            let strikes = v.get("strikes").and_then(|s| s.as_u64()).unwrap_or(1).min(u32::MAX as u64) as u32;
            Some((std::path::PathBuf::from(dir), SavedBlock { strikes, until }))
        })
        .collect()
}

/// Write the blocks (replacing the file).
pub fn save_blocks(
    path: &std::path::Path,
    blocks: &std::collections::BTreeMap<std::path::PathBuf, SavedBlock>,
) -> std::io::Result<()> {
    let m: serde_json::Map<String, serde_json::Value> = blocks
        .iter()
        .map(|(d, b)| (d.display().to_string(), serde_json::json!({"until": b.until, "strikes": b.strikes})))
        .collect();
    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir)?;
    }
    let tmp = path.with_extension("json.tmp");
    std::fs::write(&tmp, serde_json::Value::Object(m).to_string())?;
    std::fs::rename(tmp, path)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn blocks_survive_a_round_trip() {
        let dir = std::env::temp_dir().join(format!("cm-blocks-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        let path = dir.join("usage-blocks.json");
        assert!(load_blocks(&path).is_empty());
        let mut m = std::collections::BTreeMap::new();
        m.insert(std::path::PathBuf::from("/h/.claude"), SavedBlock { strikes: 2, until: 1_791_950_000 });
        m.insert(std::path::PathBuf::from("/h/My Claude"), SavedBlock { strikes: 1, until: 5 });
        save_blocks(&path, &m).unwrap();
        assert_eq!(load_blocks(&path), m);
        std::fs::write(&path, "garbage").unwrap();
        assert!(load_blocks(&path).is_empty());
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn ladder_then_cap_then_reset() {
        let mut b = Backoff::default();
        assert!(!b.active(0));
        assert_eq!(b.on_miss(1000, None), 600);
        assert!(b.active(1599));
        assert!(!b.active(1600));
        assert_eq!(b.remaining(1300), Some(300));
        assert_eq!(b.on_miss(1600, None), 1200);
        assert_eq!(b.on_miss(2800, None), 1800);
        assert_eq!(b.on_miss(4600, None), 1800, "capped");
        assert_eq!(b.strikes(), 4);
        b.on_success();
        assert_eq!(b, Backoff::default());
        assert_eq!(b.on_miss(9000, None), 600, "a success starts the ladder over");
    }

    #[test]
    fn retry_after_never_shortens_the_floor() {
        let mut b = Backoff::default();
        assert_eq!(b.on_miss(0, Some(589)), 600);
        let mut b = Backoff::default();
        assert_eq!(b.on_miss(0, Some(900)), 910, "a longer hint wins, plus a margin");
        assert_eq!(b.until(), Some(910));
        let mut b = Backoff::default();
        assert_eq!(b.on_miss(0, Some(-5)), 600);
    }
}
