# Usage statistics screen — design

Date: 2026-07-15
Status: approved, not yet implemented
Follows: 2026-07-15-codex-usage-design.md

## Goal

A `/stats`-style screen on the device, reached by tapping the title. Per-provider:
tapping the title on the Claude tab shows Claude's stats, on the Codex tab shows
Codex's. Also rewires the tap targets so the splash only opens from the logo.

## Findings that drive the design

Verified on this machine 2026-07-15.

### Claude: `~/.claude/stats-cache.json` is the exact `/stats` source

Confirmed by reproducing the numbers: `longestSession.duration = 857580939` ms
renders as **9d 22h 13m**, matching Claude Code's own `/stats` output to the
minute. Shape (v4):

```
version, lastComputedDate, totalSessions, totalMessages, firstSessionDate
dailyActivity[]   : { date, messageCount, sessionCount, toolCallCount }
dailyModelTokens[]: { date, tokensByModel }
modelUsage{}      : model -> { inputTokens, outputTokens, cacheReadInputTokens,
                               cacheCreationInputTokens, ... }
longestSession    : { sessionId, duration(ms), messageCount, timestamp }
hourCounts{}      : hour -> count
```

Derived daemon-side: total tokens = Σ(inputTokens+outputTokens) over `modelUsage`
(excludes cache tokens — this reproduces the `/stats` figure); favorite model =
highest total; streaks / active days from `dailyActivity` dates; Dune ratio =
tokens ÷ 245_000.

**The cache is recomputed by Claude Code on its own schedule** — `lastComputedDate`
was 2026-07-14 while the wall clock said 2026-07-15. The device therefore shows
what `/stats` shows, staleness included. We do **not** recompute from the ~999
session JSONLs: slow, and it would drift from what `/stats` reports.
**Read-only** — Claude Code owns this file.

### Codex: no cache exists; aggregate the rollout JSONLs

No equivalent file. Stats come from `${CODEX_HOME:-~/.codex}/sessions/**/rollout-*.jsonl`
plus `archived_sessions/*.jsonl`. Per session take the LAST `token_count` event's
`info.total_token_usage` (it is cumulative), and the first/last `timestamp` for
duration. Model appears as `payload.model` (observed: `gpt-5.6-sol`).

**Total tokens MUST be `(input_tokens - cached_input_tokens) + output_tokens`.**
Codex's `input_tokens` *includes* `cached_input_tokens`, whereas Claude's
`/stats` total counts only non-cache `inputTokens + outputTokens`. Summing
Codex's raw `input_tokens + output_tokens` gives 218.8m against Claude's 51.6m —
a meaningless 4x "win" that is really 207.5m of re-sent cached context. The
like-for-like figures are Claude 51.6m (41.6m output) vs Codex 11.2m (1.2m
output). Getting this wrong makes the two tabs silently incomparable.

Measured: 58 files, **0.15s** to aggregate the lot — no caching needed; do it
inline each stats cycle.

**Codex history is only 2 days deep** (all files dated 2026-07-14/15). Its
heatmap and streaks are therefore near-empty. Accepted: show the real, sparse
graph rather than hide it or fake depth. Current values: 58 sessions, 11.2m
comparable tokens (1.2m output), longest session 47m.

### Existing constraints

- Firmware RX buffer 512 B; bash daemon writes with-response so BlueZ long-writes.
- Firmware parses one JSON per write into one struct.
- `screen_t` = SPLASH, USAGE, CODEX (`ui.h`).
- Tap today: `global_click_cb` on `usage_container` — any tap toggles splash.

## Decisions

| Decision | Choice | Why |
| --- | --- | --- |
| Stats source (Claude) | `stats-cache.json`, read-only | It IS the `/stats` source; matches exactly |
| Stats source (Codex) | aggregate rollout JSONLs | No cache exists; 0.15s is cheap |
| Transport | separate payload per provider | Both providers' stats + usage would exceed 512 B |
| Enter/exit | tap the title | Symmetric; leaves swipe for tab paging |
| Codex heatmap | show it, sparse | Honest; fills in as history accrues |

## Wire protocol

Stats travel in their **own** payload, not the usage one — usage (~150 B) plus two
providers' stats (~2×190 B) would exceed the 512 B RX buffer. One payload per
provider, on a slower cadence (`STATS_INTERVAL`, 300 s; stats move slowly).

Discriminator: `sv` present ⇒ stats payload; absent ⇒ usage payload (unchanged).

| Key | Meaning | Example |
| --- | --- | --- |
| `sv` | stats payload version | `1` |
| `p` | provider: `"c"` Claude, `"x"` Codex | `"c"` |
| `tt` | total tokens, millions (float) | `51.6` |
| `fm` | favorite model, display name | `"Opus 4.8"` |
| `ns` | sessions | `331` |
| `ls` | longest session, seconds | `857580` |
| `ad` / `as` | active days / span days | `29` / `47` |
| `cs` / `bs` | current / best streak, days | `17` / `17` |
| `la` | last active day, formatted | `"Jul 14"` |
| `dn` | Dune ratio (tokens ÷ 245k) | `223` |
| `hm` | heatmap: 49 chars `'0'`–`'4'`, oldest→newest | `"0012340..."` |

~200 B per provider. Absent/failed source ⇒ no payload for that provider ⇒ the
screen says "No stats yet". Never affects the usage payload.

## Firmware

- `data.h`: new `StatsData { bool valid; float total_tokens_m; char model[16];
  int sessions; long longest_secs; int active_days, span_days, streak, best_streak;
  char last_active[12]; int dune; char heat[50]; }` — two instances (Claude, Codex).
- `main.cpp`: `parse_json` branches on `sv`; stats fill the provider's `StatsData`
  and never touch `UsageData`.
- `ui.cpp`: `SCREEN_STATS` joins `screen_t`. It renders whichever provider the
  user came from (`stats_provider`), so tapping the title on Codex shows Codex.

## Tap targets

| Tap | Before | After |
| --- | --- | --- |
| logo (top-left) | — (inert) | splash |
| title text | — (inert) | stats ⇄ back |
| anywhere else | toggles splash | nothing |
| splash (anywhere) | back | back (unchanged) |

`logo_img` and `lbl_title` need `LV_OBJ_FLAG_CLICKABLE` (images/labels are not
clickable by default). The catch-all `global_click_cb` on `usage_container` is
removed. Swipe still pages Claude ⇄ Codex; gestures are already consumed
(`lv_indev_wait_release`) so a swipe cannot trigger these taps.

## Layout (480×480)

```
+--------------------------------+
|  logo   Stats           batt   |
+--------------------------------+
|  # # . # # # #     51.6m       |
|  # # # # . # #     tokens      |
|  # # # # # # #                 |
|  # # . # # # #     Opus 4.8    |
|  # # # # # # #     favorite    |
+--------------------------------+
|  331 sessions    17d streak    |
|  9d 22h longest  29/47 days    |
+--------------------------------+
|  ~223x more tokens than Dune   |
+--------------------------------+
```

7x7 heatmap grid, cell colour by level 0-4 from the theme palette (bar-bg for 0,
green->orange ramp above). Compact breakpoint (368x448) uses smaller cells and
fonts; same grid. Exact pixels iterated against `screenshot.sh`, not fixed here.

ASCII only in labels — the Styrene faces are built `-r 0x20-0x7E`, so any glyph
above ASCII renders as tofu (learned the hard way; see CLAUDE.md gotcha 4).

## Error handling

Missing / unreadable / malformed `stats-cache.json`, or no Codex sessions => that
provider's stats payload is simply not sent => `StatsData.valid` stays false => the
screen shows "No stats yet". The usage payload and both usage tabs are never
affected. Codex aggregation is wrapped so a single corrupt JSONL line is skipped
rather than failing the batch.

## Testing

- `daemon/tests/test_bash_stats.sh`: fixture `stats-cache.json` — total-token sum
  excludes cache tokens; favorite-model pick; streak math **including a gap that
  breaks a streak** and a single-day corpus; missing/empty/malformed cache => no
  payload. Codex aggregation: last-`token_count`-wins, corrupt line skipped.
- Firmware: screenshot both stats screens and the "No stats yet" state on the 2.16.
- All four envs compile.

## Out of scope

- Recomputing Claude stats from raw session JSONLs (use the cache).
- `hourCounts` / time-of-day chart, `dailyModelTokens` breakdown, cost figures.
- Stats on the macOS/Windows daemons.
