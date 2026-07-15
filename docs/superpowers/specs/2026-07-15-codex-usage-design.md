# Codex usage tracking — design

Date: 2026-07-15
Status: approved, not yet implemented

## Goal

Show OpenAI Codex usage on the Clawdmeter alongside Claude usage, on one screen,
without letting a Codex failure degrade the existing Claude display.

## Findings that drive the design

All verified on this machine (Linux, codex-cli 0.144.4, ChatGPT Plus) on
2026-07-15. Several widely-repeated claims about Codex usage data are wrong; the
ones below survived adversarial verification.

### Codex quota is readable, via a real usage endpoint

`~/.codex/auth.json` holds a ChatGPT OAuth token (`auth_mode: "chatgpt"`,
`OPENAI_API_KEY: null`). With `tokens.access_token` as a bearer token:

    GET https://chatgpt.com/backend-api/wham/usage   ->   HTTP 200

Response (real, captured 2026-07-15; `user_id`/`account_id`/`email` stripped):

```json
{
  "plan_type": "plus",
  "rate_limit": {
    "allowed": true,
    "limit_reached": false,
    "primary_window": {
      "used_percent": 92,
      "limit_window_seconds": 604800,
      "reset_after_seconds": 595912,
      "reset_at": 1784696820
    },
    "secondary_window": null
  },
  "credits": { "has_credits": false, "balance": "0" }
}
```

This is the endpoint the Codex CLI itself polls. It is **undocumented** — see Risks.

### Codex has ONE window, not two

`secondary_window` is `null` in the live response and in 1746/1746 session-log
snapshots on this machine. Claude draws two bars (5h + weekly); Codex draws one
(weekly). Three bars total, not four. This is the single most important
constraint on the UI.

Do not hardcode "weekly": read `limit_window_seconds`. Window slot order is not a
reliable indicator of window length.

### Rejected data sources

- **`~/.codex/sessions/**/rollout-*.jsonl` scraping.** Works — every `token_count`
  event carries `payload.rate_limits.primary.{used_percent, window_minutes, resets_at}`
  — but the data only updates when a Codex session runs, so it goes stale when
  idle. The endpoint is always current. (Note: the field is `resets_at`, a unix
  epoch. The widely-cited `resets_in_seconds` does not exist in 0.144.4 — 0
  occurrences across 1746 snapshots.)
- **`codex` CLI subcommand.** None exists. No `usage`/`limits`/`quota`/`status`;
  those parse as a prompt. `codex login status` prints only "Logged in using ChatGPT".
- **`~/.codex/app-server-control/app-server-control.sock`.** Dead inode, not a
  live daemon — `connect()` returns ECONNREFUSED (errno 111).
- **OpenAI platform usage APIs** (`/v1/organization/costs`, `/v1/organization/usage/*`).
  These cover pay-per-token API keys only and have zero visibility into
  ChatGPT-subscription Codex usage. Different product, different billing system.

### Token lifetime

`tokens.access_token` is a JWT with a **240h (10-day)** lifetime. `auth.json` also
holds a `refresh_token` and `last_refresh`; the Codex CLI rotates the token.
`tokens.id_token` has a 1h lifetime and is already expired — do not use it.

**The daemon must only ever READ `auth.json`.** Writing it would race the Codex
CLI over a credentials file and risk logging the user out. Token refresh is the
CLI's job; if the token expires, we degrade (below) until the user next runs Codex.

### Existing architecture (relevant facts)

- Claude usage does **not** come from a usage API. The daemon POSTs a 1-token
  Haiku call to `api.anthropic.com/v1/messages`, discards the body
  (`curl -D - -o /dev/null`), and reads `anthropic-ratelimit-unified-*` **headers**.
  (`daemon/claude-usage-daemon.sh:306-336`)
- Three daemons exist, split by platform, not competing: bash = Linux/systemd
  (canonical here), `claude_usage_daemon.py` = macOS/launchd,
  `claude_usage_daemon_windows.py` = Windows.
- Firmware parses 13 JSON keys, **all with defaults**, so any key may be omitted.
  (`firmware/src/main.cpp:109-122`, struct at `firmware/src/data.h:4-19`)
- `ble.cpp` is transport only — zero JSON parsing. RX buffer is 512 bytes and
  clamps rather than overflows. Writes are rejected unless encrypted and from the
  locked owner address. (`firmware/src/ble.cpp:15`, `:75`, `:262-286`)
- `screen_t` has only `SCREEN_SPLASH` and `SCREEN_USAGE`. The usage screen has
  three sub-views chosen by `update_view_state()` (`ui.cpp:557-574`):
  pair / idle / usage, with `DATA_FRESH_MS = 90000`.
- `compute_layout()` (`ui.cpp:52-89`) has exactly one breakpoint: `height >= 460`
  (large, 480x480) else compact (368x448). `margin = 20`, `title_y = 30`.

## Decisions

| Decision | Choice | Why |
| --- | --- | --- |
| Display | One screen, 3 bars | Glanceable; no button press |
| Daemon | Extend `claude-usage-daemon.sh` | Firmware enforces a single BLE writer |
| Platforms | Bash only | Ships today; mac/Windows degrade to Claude-only |
| Boards | Design all, verify 2.16 | Shared `ui.cpp`; only the 2.16 is on hand |
| Codex down | Omit keys, show 2 bars | Codex must never degrade Claude |

## Architecture

```
claude-usage-daemon.sh  (one process, one BLE link, one payload)
  |
  +-- poll Anthropic -> 1-token Haiku call, read ratelimit headers -> s, sr, w, wr
  +-- poll Codex     -> read ~/.codex/auth.json, GET wham/usage    -> cx, cxr, cxw
  |
  +-- merge -> ONE json -> RX char ...0002 -> firmware -> 3 bars
```

No new process, characteristic, connection, or systemd unit.

## Wire protocol

Three new keys, all optional with defaults, consistent with the existing
every-key-omittable design. An old daemon against new firmware renders 2 bars; a
new daemon against old firmware ignores the extra keys.

| Key | Meaning | Type | Default | Example |
| --- | --- | --- | --- | --- |
| `cx` | Codex used_percent | float | `-1` | `92` |
| `cxr` | minutes until reset | int | `-1` | `9932` |
| `cxw` | window length, minutes | int | `10080` | `10080` |

`cx >= 0` is the validity flag. No separate boolean key.

Size: payload grows ~20 bytes (75 -> ~95). The binding constraint is the macOS
python path (write-without-response, ~182-byte ceiling); worst case there
(Enterprise + clock + chime + Codex) is ~150 bytes. The bash path uses
write-with-response, where BlueZ transparently long-writes. No chunking needed.

## Daemon changes (`daemon/claude-usage-daemon.sh`)

1. `read_codex_token()` — extract `.tokens.access_token` from
   `${CODEX_HOME:-$HOME/.codex}/auth.json`. Read-only. Return empty on any
   problem. Honour `CODEX_HOME` (it overrides the default path).
2. `poll_codex()` — `curl` the endpoint with a short timeout (5s connect / 10s
   total) so a hung endpoint cannot stall the 60s loop. Parse with `python3`
   (already a dependency — the Enterprise path shells out to it for date math).
3. Window selection — choose the **most-used non-null** window among
   `primary_window` / `secondary_window`. Emit its `used_percent`,
   `reset_after_seconds / 60`, and `limit_window_seconds / 60`.
   `# ponytail: most-used window; revisit if OpenAI ships a real 5h+weekly pair`
4. Merge `cx`/`cxr`/`cxw` into the existing payload builder as an additive
   fragment, exactly like the existing clock and chime fragments
   (`claude-usage-daemon.sh:289-327`).

Config: no new keys. Codex is auto-detected by the presence of a readable token.

## Firmware changes

- `firmware/src/data.h` — add to `UsageData`:
  `float codex_pct; int codex_reset_mins; int codex_window_mins; bool codex_valid;`
- `firmware/src/main.cpp:parse_json()` — three `doc[...] | default` lines;
  `codex_valid = (codex_pct >= 0)`.
- `firmware/src/ui.cpp` — third panel via the existing `make_usage_panel()`;
  `compute_layout()` gains 3-panel geometry at both breakpoints; label derived
  from `codex_window_mins` (`>= 10080` -> "weekly", else "Nh"); reuse the existing
  reset formatter (`9932` -> `6d 21h`).

No new fonts: Styrene 28/24/20 are already compiled in and fit the shorter panels.

## Layout

Panels shrink to fit three plus the `Mustering...` status line:

- large (480x480): `usage_panel_h` 150 -> ~100, gap 16 -> ~12
- compact (368x448): `usage_panel_h` 130 -> ~85, gap 12 -> ~10

Exact pixels are iterated against `screenshot.sh`, not fixed here. Panel
internals (`bar_y`, `reset_y`, pct font) compress proportionally.

When `codex_valid` is false the third panel is hidden and the remaining two
return to today's full-size geometry — i.e. the current screen, unchanged.

## Error handling

Every Codex failure — missing `~/.codex`, missing/unreadable token, 401, 404,
timeout, malformed JSON, both windows null — takes one path: **omit `cx`, log
once, continue**. The Anthropic poll is never gated on the Codex poll, and no
Codex failure can extend the poll cycle beyond its timeout.

Ignore `limit_id: "premium"` records if the JSONL path is ever revisited; only
`limit_id: "codex"` carries real quota.

## Testing

- One runnable check for the daemon's window-selection + payload-merge logic,
  following the existing `daemon/tests/` pattern. Covers: primary-only,
  both-windows (picks most-used), both-null, missing token, HTTP error.
- `screenshot.sh` on the 2.16 for three states: 3-bar live, 2-bar fallback,
  idle/stale.
- Compile all four envs (`waveshare_amoled_216`, `_18`, `_216_c6`, `_18_c6`).

## Risks

- **The endpoint is undocumented.** OpenAI can change or remove
  `/backend-api/wham/usage` without notice. Mitigation: the degradation path is
  the same as any other failure — Codex silently drops to 2 bars, Claude is
  unaffected. The JSONL rollout files remain a documented fallback if needed.
- **Compact layout is unverified on hardware.** The 368x448 3-bar geometry is
  derived, not eyeballed. C6 boards cannot be screenshotted (`LV_USE_SNAPSHOT=0`).
- **Token expiry when Codex is idle >10 days** drops the Codex bar until the user
  next runs `codex`. Accepted — refreshing it ourselves would race the CLI.

## Out of scope

- Codex reset chime.
- Codex bar on the Enterprise layout (`acct: "ent"` keeps its spending view).
- macOS / Windows daemons.
- `credits`, `spend_control`, `code_review_rate_limit` fields.

## Documentation fixes (included, they mislead future sessions)

`CLAUDE.md` is stale in two verified ways:

1. It describes a "3-screen UI (splash, usage, bluetooth)" and instructs setting
   the boot screen to `SCREEN_CONTROLLER` / `SCREEN_BLUETOOTH`. Those identifiers
   do not exist and would not compile (`ui.h:5-9`).
2. Its "Daemon / host side" section claims the daemon connects by name-scanning
   and self-heals with `bluetoothctl remove`. Both are false and deliberately so
   — `find_system_device_mac()` only ever targets an already-paired/connected
   device, and `connect_device()` explicitly does NOT remove. This cost real time
   during setup: on an unpaired host the daemon backs off forever with
   "not scanning" until the device is paired manually.
