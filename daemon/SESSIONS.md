# Live session awareness — host setup

Shows your open Claude Code chats on the device: what each one is doing, how
full its context window is, todo/subagent counts — and, most importantly,
whether any of them is waiting on you. Design and rationale live in issue
[#135](https://github.com/HermannBjorgvin/Clawdmeter/issues/135).

**Off by default.** The session data comes from a Claude Code hook integration
you have to install; without it the device behaves exactly as it does today.

## How it works

```
Claude Code sessions ──HTTP hooks (127.0.0.1)──▶ clawdmeter_sessions.py
   transcripts *.jsonl ─────context tokens─────▶   (state machine + sort)
   <config-dir>/sessions/<pid>.json ─liveness──▶        │
                                                        ▼ atomic write on change
                                            ~/.clawdmeter/sessions.json
                                                        │ 5 s tick, on change
claude-usage-daemon.sh ──BLE GATT SS …0005─────────────▶ Clawdmeter firmware
```

The sidecar (`daemon/clawdmeter_sessions.py`, Python 3 stdlib only) listens on
loopback for hook POSTs, keeps a table of live sessions, sorts it
attention-first, fits it to a byte budget, and writes the finished wire payload
to `~/.clawdmeter/sessions.json`. The bash daemon ships that payload to the SS
GATT characteristic whenever the file's content changes. The listener is a
read-only observer: it answers `204 No Content` and can never block a tool call
or approve a permission.

## Setup (Linux)

`./install.sh` prompts for all of this (default yes). Manually:

1. **Config** — in `~/.config/claude-usage-monitor/config`:

   ```ini
   hook_port = 45999
   # context_window_k =          # optional: pin the context window (kilotokens)
   # sessions_budget_bytes = 180 # optional: payload byte budget
   ```

2. **Hook block** — merge into `~/.claude/settings.json` (and any other Claude
   config dir you use). The helper is idempotent and backs up the file first:

   ```bash
   python3 daemon/clawdmeter_sessions.py --install-hooks ~/.claude/settings.json http://127.0.0.1:45999/
   ```

   Or add it by hand — this is the exact block:

   ```json
   {
     "hooks": {
       "SessionStart":       [{ "hooks": [{ "type": "http", "url": "http://127.0.0.1:45999/", "async": true, "timeout": 5 }] }],
       "UserPromptSubmit":   [{ "hooks": [{ "type": "http", "url": "http://127.0.0.1:45999/", "async": true, "timeout": 5 }] }],
       "PreToolUse":         [{ "hooks": [{ "type": "http", "url": "http://127.0.0.1:45999/", "async": true, "timeout": 5 }] }],
       "PostToolUse":        [{ "hooks": [{ "type": "http", "url": "http://127.0.0.1:45999/", "async": true, "timeout": 5 }] }],
       "PostToolUseFailure": [{ "hooks": [{ "type": "http", "url": "http://127.0.0.1:45999/", "async": true, "timeout": 5 }] }],
       "PermissionRequest":  [{ "hooks": [{ "type": "http", "url": "http://127.0.0.1:45999/", "async": true, "timeout": 5 }] }],
       "PermissionDenied":   [{ "hooks": [{ "type": "http", "url": "http://127.0.0.1:45999/", "async": true, "timeout": 5 }] }],
       "Notification":       [{ "hooks": [{ "type": "http", "url": "http://127.0.0.1:45999/", "async": true, "timeout": 5 }] }],
       "MessageDisplay":     [{ "hooks": [{ "type": "http", "url": "http://127.0.0.1:45999/", "async": true, "timeout": 5 }] }],
       "Stop":               [{ "hooks": [{ "type": "http", "url": "http://127.0.0.1:45999/", "async": true, "timeout": 5 }] }],
       "StopFailure":        [{ "hooks": [{ "type": "http", "url": "http://127.0.0.1:45999/", "async": true, "timeout": 5 }] }],
       "PreCompact":         [{ "hooks": [{ "type": "http", "url": "http://127.0.0.1:45999/", "async": true, "timeout": 5 }] }],
       "PostCompact":        [{ "hooks": [{ "type": "http", "url": "http://127.0.0.1:45999/", "async": true, "timeout": 5 }] }],
       "SubagentStart":      [{ "hooks": [{ "type": "http", "url": "http://127.0.0.1:45999/", "async": true, "timeout": 5 }] }],
       "SubagentStop":       [{ "hooks": [{ "type": "http", "url": "http://127.0.0.1:45999/", "async": true, "timeout": 5 }] }],
       "SessionEnd":         [{ "hooks": [{ "type": "http", "url": "http://127.0.0.1:45999/", "async": true, "timeout": 5 }] }]
     }
   }
   ```

   If your settings.json already has hooks, keep them — Clawdmeter's entries
   are appended alongside, not instead. New sessions pick hooks up on start;
   already-running sessions keep their old hook set.

3. **Run the sidecar** — `./install.sh` installs and enables the
   `clawdmeter-sessions` systemd user unit:

   ```bash
   systemctl --user start clawdmeter-sessions
   journalctl --user -u clawdmeter-sessions -f
   ```

   Or run it in a terminal: `python3 daemon/clawdmeter_sessions.py`

4. **Verify** — open a Claude Code session, then:

   ```bash
   curl -s http://127.0.0.1:45999/          # current wire payload (loopback debug)
   cat ~/.clawdmeter/sessions.json          # what the daemon will ship
   ```

## Wire format

The payload is `{"ss":[...]}` with one positional row per session, already
sorted attention-first (waiting, working, idle; most recent first within each):

```
[sid, label, state, ctx, elapsed_s, model, tool, ntools, nagents, tdone, ttotal, tok]
```

| # | Field | Meaning |
| - | --- | --- |
| 0 | `sid` | 2 hex chars, stable for the session's life (keys the reorder animation) |
| 1 | `label` | Display name, already middle-elided to fit the budget |
| 2 | `state` | State code 0–10 (issue #135 §3; append-only) |
| 3 | `ctx` | Context window used, percent; `-1` = unknown (firmware hides the bar) |
| 4 | `elapsed_s` | Seconds in the current state at write time |
| 5 | `model` | `0` unknown, `1` opus, `2` sonnet, `3` haiku, `4` fable |
| 6 | `tool` | `0` other/none, `1` Bash, `2` Read, `3` Edit, `4` Write, `5` Grep, `6` Glob, `7` Task, `8` WebFetch, `9` WebSearch |
| 7 | `ntools` | OPEN tool calls (concurrent, not cumulative) |
| 8 | `nagents` | Subagents currently in flight |
| 9–10 | `tdone` / `ttotal` | Todo counts; badge hidden when `ttotal` is 0 |
| 11 | `tok` | Context tokens used, in 1k units (rounded to nearest) — the absolute number behind `ctx`, from the same transcript read. `-1` exactly when `ctx` is `-1` |

Fields are append-only: firmware ignores indices it doesn't know, and new
fields only ever go on the end.

## Config reference

| Key | Default | Meaning |
| --- | --- | --- |
| `hook_port` | unset | Loopback port for the hook listener. **Unset = feature off** — the sidecar exits, the daemon sends nothing. |
| `context_window_k` | unset | Pin the context window in kilotokens (e.g. `200`, `1000`). Blank = heuristic: 200k default, 1M on a `[1m]` model marker, snap up to the next 1M multiple when observed usage exceeds the assumption. A pinned value disables the snap-up. |
| `sessions_budget_bytes` | `180` | Byte budget for the fitted payload. Labels middle-elide down to an 8-char floor first, then the least-urgent rows drop from the tail. Keep below the BLE MTU the device negotiates. |

The sidecar also honors `config_dirs` (shared with the daemons) to find session
rosters and transcripts across several Claude config dirs.

## Notes

- **Privacy/security.** Hook payloads contain prompt and response text, so the
  listener binds `127.0.0.1` only and rejects non-loopback peers. Nothing from
  the payload text ever reaches the device — only names, states, and counts.
- **Liveness.** Sessions are considered alive while their roster entry
  (`<config-dir>/sessions/<pid>.json`) points at a running process — not on an
  activity timeout, so a chat parked on a permission prompt survives
  indefinitely. Roster absence is acted on after a 30 s grace; a 6 h staleness
  sweep backstops an unreadable roster.
- **Context % is a heuristic** read from the transcript tail
  (`input_tokens + cache_read_input_tokens + cache_creation_input_tokens`),
  re-read on SessionStart/Stop/PostCompact. Good for a glanceable bar, not for
  quoting numbers.
- **macOS / Windows** — the Python daemons don't ship session data yet. The
  sidecar is importable as a library (`SessionTable`, `fit_payload`, …) for
  that integration; this round wires up Linux only.
- **Firmware support** — the device needs firmware with the SS characteristic
  (`…0005`) and a board with the session-views capability. Older firmware just
  never sees the data; the daemon stays silent about it.
