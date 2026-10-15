# Desktop app and legacy daemon parity

This reference maps the retired Linux bash daemon, macOS Python daemon,
and Windows Python tray to the Rust desktop app. Legacy sources and
installers remain in git history. Rust paths below are relative to
`desktop/crates/`.

Use the README for installation. This document records upgrade details,
protocol behavior, and differences that matter when diagnosing an upgrade.

Legend: **L** bash (Linux), **M** Python (macOS), **W** Python (Windows).
"—" means that daemon didn't do it.

## Replacing a legacy daemon

The desktop app replaces the bash and Python daemons. It uses only the
current state folder and ignores the legacy directory. Set up accounts
and preferences in the app.

1. Install the desktop app. If **Settings** reports a legacy service,
   disable it there so only one process talks to the device. On Windows,
   also quit the Python tray or sign out to stop the running process.
2. Open **Hooks** and update entries that use `attention-hook.sh`. Review
   the file changes before saving.
3. Remove the legacy checkout's `daemon/.venv` if you no longer need it.

## Usage source

The legacy daemons sent a one-token Haiku request and read usage from
response headers. The app has Claude Code do the same while an account's
5-hour window is known to be running (`claude -p --output-format
stream-json --verbose`, its `rate_limit_event`; `clawdmeter-core/src/probe.rs`),
and runs `claude --output-format json -p /usage` otherwise, since a message
would start a window. Otherwise it asks `GET /api/oauth/usage` with the token
Claude Code stored (`clawdmeter-core/src/oauth.rs`), read-only like the old
daemons and never refreshed; `claude -p /usage` is the last resort when no
token is readable.

| Behaviour | Old daemons | Rust | Notes |
|---|---|---|---|
| Read the OAuth token | L: `<dir>/.credentials.json`, isolating `claudeAiOauth` · M: same file, else Keychain `Claude Code-credentials` (hex-dump decode) · W: `CLAUDE_CREDENTIALS_PATH`, `CLAUDE_CONFIG_DIR`, `~/.claude`, `%LOCALAPPDATA%\Claude`, `%APPDATA%\Claude` | Read, never refreshed, for the idle source: on macOS the Keychain item Claude Code uses for that dir first (`Claude Code-credentials`, `-<sha256(dir)[..8]>` for a non-default dir; hex-dump decode), then `<dir>/.credentials.json` (`claudeAiOauth`); elsewhere the file | The probe (Claude Code itself) reads its own credentials; an expired or refused token makes the idle source run `claude -p /usage`, which renews it. |
| Never refresh the token ("free-ride") | M, W | Same by construction | The CLI owns refresh. |
| Session % (`s`) | `round(5h-utilization × 100)` | `Current session: N% used` → `s` (`clawdmeter-core/src/usage.rs`) | The CLI floors utilization before printing, so `s` can read one point lower than the old `round()`. |
| Weekly % (`w`) | `round(7d-utilization × 100)` | `Current week (all models)` | |
| Model weekly lines | — | Parsed into `UsageReading::models`, not sent | The device has two meters. |
| Reset countdowns (`sr`, `wr`) | Minutes to the header epoch, rounded, clamped at 0; 0 when the header is missing | Minutes to the parsed reset (`payload.rs` `minutes_until`), clamped at 0; **-1** when there is no window or no reset | Firmware reads `sr < 0` as "window not started" (the director's screensaver gate). Reset text with a year (`Jan 2, 2027, 9:59pm`) is parsed; the CLI prints one only across a year boundary. |
| Status (`st`) | Header value: `5h-status` (Pro) or `unified-status` (Enterprise): `allowed` / `allowed_warning` / `rejected`; `unknown` when missing | `allowed`, `limited` (session, week or spend cap at 100 % and no extra usage), `overage` (the CLI says "using your overages") | The firmware stores `st` and never shows it. |
| Account kind (`acct`) | `pro` when the 5h header exists, else `ent` | `pro`, or `ent` when the account only has a `Spend limit:` line (`UsageReading::is_spend_only`) | |
| Enterprise spend % | `round(overage-utilization × 100)` as `s` | `Spend limit: $X / $Y spent <period> (N%)` or `Spend limit: N% used` → `s` | Format taken from the `/usage` command in Claude Code 2.1.296. Spend amounts aren't sent. |
| Billing period (`tp`, `pd`, `rd`) | Period = one calendar month before the overage reset (day clamped), host-local; `tp` clamped 0–100; `{tp:0, pd:30, rd:""}` on a missing or garbage reset | `payload::billing_period`: same rule, plus `today` = 1 day and `this week` = 7 days when the line names the period | Ported cases: month clamping, January rollover, before/after clamps, host-local date. |
| Seat-based Enterprise / Team | Same 5h/7d headers as Pro | Same `Current …` lines | Week-only output (no session line) works: `sr = -1`. |
| Account with no limits in `/usage` | Whatever the headers said | A miss (`UsageError::NoUsageLines`, or `RateLimited` when Claude Code's debug log shows a 429); the account backs off; the device shows stale after three misses or once the last good reading is over 20 min old | This is what a rate-limited usage endpoint looks like: Claude Code drops the `Current …` lines and remembers the 429 for ~600 s per token. Missing usage stays missing. |
| Logged out / expired token | M, W: `{"ok":false}` when every dir fails auth · L: logged, nothing sent | `{"ok":false}` once per poll when every account fails with a login or not-a-subscription error (`engine.rs`) | |
| Transient failure (network, 429, 5xx) | M, W: silent retry · L: logged | Per-account backoff (`clawdmeter-core/src/backoff.rs`): 10 min after a miss, then 20, then 30 (cap), reset on success; device refresh requests and "Refresh now" don't bypass it. The account keeps its last values (and stays shown) and goes stale after three misses in a row, or once its last good reading is over 20 min old. Accounts shows "Retrying" with the wait | Misses never feed the rate math. The 10-minute floor is Claude Code's own 429 memory; a longer "not asking again for Ns" from `<config dir>/debug/latest` wins. A missing CLI or a login problem doesn't back off; it retries after 2 min. |
| Several accounts (`config_dirs`) | L, M; W: one account | Every listed dir is polled; the plan whose session % rose most recently is shown, else the highest (`selector.rs`) | Ported tests: startup pick, switch on rise, sticky, reset is not activity, larger rise wins. |
| Default account | `~/.claude` (W: `CLAUDE_CONFIG_DIR` when set) | `~/.claude`, or `$CLAUDE_CONFIG_DIR` when the config has no `config_dirs` (`Settings::apply_env_config_dir`) | Only the daemon applies the env default; the settings window shows and saves the file's value. |

## Config file

Config lives at `~/.config/clawdmeter/config` on Linux and
macOS, `%LOCALAPPDATA%\Clawdmeter\config` on Windows; `key = value`, `#`
comments, last key wins, case-insensitive keys (`clawdmeter-core/src/config.rs`).
The app uses only the current state folder and ignores the legacy directory.
The app re-reads config every 5 s and the settings window edits it in place,
keeping comments and unknown keys.

| Key | Old daemons | Rust | Notes |
|---|---|---|---|
| `config_dirs` | L, M | Yes | |
| `poll_interval` | L only (min 10, re-read every tick); M, W fixed at 60 s | Yes (min 10; the scheduler never polls an account more than every 120 s) | When set, it replaces both the 3-minute working cadence and the 5-minute no-hooks cadence. Unset, polling follows the hooks (see below). |
| `heartbeat_interval` | L only (default 60, min 10; off when ≥ `poll_interval`) | Yes (default 60, min 10) | Always on: with 15-minute idle polls the heartbeat is what keeps the device's 90 s freshness window open. |
| `reset_chime` | L, M, W (as `chime`) | Yes (`c:1`; `chime` is read when `reset_chime` is absent) | |
| `alert_chime` | L, M (as `beep`) | Yes (`b:1` with an alert; `beep` is read when `alert_chime` is absent) | |
| `clock` (`off`/`auto`/`12`/`24`) | L, M, W | Yes (`t`, `tf`) | `auto`: macOS `AppleICUForce24HourTime`/`12HourTime`, else `locale -k LC_TIME`; Windows `iTime` (`clawdmeter-core/src/platform.rs`). |
| `desktop_chime`, `alert_on`, `release_repo`, `claude_path`, `update_check`, `onboarding_done` | — | New | The ding on this computer, which alert kinds get through, flasher source, CLI location override, daily firmware check, setup-wizard completion. |

## Environment

| Variable | Old daemons | Rust |
|---|---|---|
| `POLL_INTERVAL` | L: seeds the default (validated like the config) | Same |
| `DEVICE_MAC` | L: pin the device | `CLAWDMETER_BLE_ADDRESS` (every OS) |
| `CLAWDMETER_BLE_ADDRESS` | W: skip the PnP lookup | Tried first on every OS |
| `CLAUDE_CONFIG_DIR` | W: token location | Default account when `config_dirs` is unset |
| `CLAUDE_CREDENTIALS_PATH` | W | Dropped (no token handling) |
| `CLAWDMETER_LOG`, `CLAWDMETER_NO_TRAY` | — | New: log level; status window instead of a tray icon |

## Polling and heartbeat

| Behaviour | Old daemons | Rust | Notes |
|---|---|---|---|
| Poll cadence | Every 60 s (L: `poll_interval`), 5 s loop tick | Event-driven and per account (`cadence.rs`). Shown account: at once on a prompt, then every 3 min while working; once ~10 s after a Stop; every 5 min with no hooks or before 10 quiet minutes; every 15 min after. Other accounts: every 10 min. Never twice within 2 min per account, whatever the trigger | `/usage` hits a rate-limited endpoint, so the budget is well under the old 60 s. 0.5 s loop tick, so hook events still reach the device within about a second. |
| Device refresh request (REQ `0x01`) | L: dbus-monitor + flag file · M, W: notify callback | Notify stream opened before subscribing, subscribe bounded at 10 s (`clawdmeter-ble/src/lib.rs`) | Polls every account at once, except one polled in the last 2 min or backing off. |
| Heartbeat replay | L: replays the last payload with `sr`/`wr` aged by whole minutes and `t` advanced | Rebuilds the payload from the absolute reset times (exact) and marks it `rp:1` (`engine.rs`) | `rp` keeps replays out of the firmware's rate math. Stops once the shown account is stale, so the device can show it. |
| "Neither interval ≤ 80 s" warning | L | Not ported | The heartbeat can't be disabled any more. |

## Attention alerts and hooks

| Behaviour | Old daemons | Rust | Notes |
|---|---|---|---|
| Hook command | `daemon/attention-hook.sh alert\|clear\|event` (needs `jq`) | `clawdmeter hook alert\|clear\|event [event]` (`clawdmeter-app/src/hooks/mod.rs`) | Same files byte for byte; no bash or jq, so it runs on Windows. One deliberate difference: an empty stdin or a session id containing `/`, `\` or `..` writes nothing (the script wrote an empty `attention`, or touched a file outside the state dir). |
| Hook installation | Hand-edited `settings.json` (README) | Hooks: per-account preview, `.bak`, updates `attention-hook.sh` entries (`clawdmeter-app/src/hooks/install.rs`) | |
| Alert file (`attention`, `{"m","s","k"}`; `k` is the alert kind) | L, M; W: — | `clawdmeter-core/src/attention.rs` | Older than 60 s when first seen: dropped. Bare `touch`: alert without a message. |
| Repeat alert from the waiting session | L, M: ignored | Ignored | AskUserQuestion notifies twice. |
| `clear-<sid>` | Ends the alert only for the alerting session and only when newer (ns mtimes) | Same | |
| Message to ASCII, 120 chars | L, M | Same (`to_ascii`) | The device gets a fixed line per alert kind instead (`AlertKind::device_text`); only the test alert sends its own text. |
| Alert write | L: inside an aged replay · M: inside a fresh poll | Inside a replay (`rp:1`), or the poll if one is due | |
| Message too long for one write | M: trims `m`, then drops it | Same (`Payload::encode`, 500-byte cap; firmware buffer is 512) | |
| Leftover alert files on connect | L, M: deleted | Deleted | |
| Fast clock (`events.log` → `a`, `e`/`es`, `n`) | The hook wrote `events.log`; no daemon read it | `clawdmeter-core/src/events.rs` | Activity input for the director. |

## Bluetooth

| Behaviour | Old daemons | Rust | Notes |
|---|---|---|---|
| Never scan by name | All | Only OS-known devices: custom service, or HID service plus exact name (`known_devices`, `accept`) | A filtered scan exists for first-time setup (`LinkOptions::allow_scan`), off by default. |
| Linux: paired/connected only | `bluetoothctl devices Paired/Connected` | BlueZ `Device1.Paired` check | |
| Linux: `bluetoothctl trust` before connect | L | `Device1.Trusted = true` after connect (`bluez::trust`) | |
| Connect failure | L: drop cached MAC, never unpair · M: skip that handle once · W: 3 attempts, 2 s apart | Drop the cache if it named that device and try every other candidate; never unpair on Linux | |
| Address cache `ble-address` | L: MAC · M: MAC or CoreBluetooth UUID; malformed discarded | Same (`AddressCache`) | |
| macOS: device held by the OS as a keyboard | `retrieveConnectedPeripheralsWithServices` (custom service, then HID + name) | btleplug `retrieve_peripherals` with the same two services | |
| macOS: connect / subscribe timeouts | 20 s / 10 s | 20 s / 10 s (`LinkOptions`) | Discovery is bounded at 15 s too. |
| macOS: stale bond (CBError 15, "failed to encrypt") | `blueutil --paired` + `--unpair`, 8 s timeout, permission hint | Same (`macos_unpair_stale_bond`) | blueutil stays optional; without it the log says how to forget the device. |
| Windows: bonded device not connected right now | PnP `BTHLE\DEV_<mac>` lookup, connect by address | `CLAWDMETER_BLE_ADDRESS`, cache, then the same PnP lookup → `add_peripheral` (`bonded_by_address`) | |
| Windows: uncached GATT discovery | `use_cached_services=False` | btleplug's WinRT discovery is uncached | |
| Half-open "zombie" link | W: drop after 1 failed write | Drop after 2 failed writes in a row (`WriteHealth`) | |
| Reconnect backoff | L: 1→60 s · W: 1→60 s searching, 1→8 s after a drop | 1→15 s, reset on every connect; a drop retries after 2 s; the OS reporting the device (connected or advertising) makes the next attempt due at once (`clawdmeter-ble/src/reconnect.rs`) | |
| Clean GATT disconnect on quit | W | Yes (`daemon.rs` on shutdown) | |
| Write type | M, W: without response, at most the MTU-derived size | With response, at most 500 bytes | |

## Start at login, install and uninstall

| Behaviour | Old daemons | Rust | Notes |
|---|---|---|---|
| Linux | `install.sh`: dependency check, systemd user unit `claude-usage-daemon.service` | Settings: XDG autostart entry for the tray, or `clawdmeter.service` for `--daemon` (`clawdmeter-core/src/autostart.rs`) | |
| macOS | `install-mac.sh`: venv, LaunchAgent `com.user.claude-usage-daemon`, Bluetooth prompt priming, blueutil | LaunchAgent `com.clawdmeter.app` (same KeepAlive / ThrottleInterval). The release ships `Clawdmeter.app` with `NSBluetoothAlwaysUsageDescription` so the prompt appears and the agent isn't killed | |
| Windows | `install-windows.ps1`: venv, `HKCU\…\Run\Clawdmeter` → `pythonw tray_windows.py`; refused `\\wsl$` paths | Same Run value name pointing at `clawdmeter.exe`; `is_enabled` only counts it when it's ours | No Python, so no WSL path issue. |
| Old daemon still installed | — | Logged at start-up; Settings › "Disable legacy service" stops it and removes its unit / plist / Run value (`autostart::disable_legacy`) | A running Python tray keeps running until quit or logout. |
| Single instance | W: named mutex | Lock file on every OS (`clawdmeter-app/src/ipc.rs`) | |
| Installer config prompts (accounts, clock, chime, beep) | `install.sh`, `install-mac.sh` | Accounts / Device | The old installers auto-detected `~/.claude*` dirs; the app asks the user to add folders. |

## Tray, logs, flashing

| Behaviour | Old daemons | Rust | Notes |
|---|---|---|---|
| Tray | W only: green/amber/red bubble, "Connected · last update HH:MM", Start at login, Quit | Every OS: status, account and both meters, Refresh usage, Mute chimes, Open Clawdmeter, Flash firmware (Update firmware when a newer release exists), Settings, Quit (`clawdmeter-app/src/tray/`) | |
| Error notification | W: toast when entering the error state | None; the tray state and status window show it | |
| Daemon thread crash | W: supervised, restarted with backoff | Release builds abort on panic; the LaunchAgent and `clawdmeter.service` restart it, the XDG entry and Run value don't | |
| Log file | L: journal · M: `~/Library/Logs/claude-usage-daemon.{out,err}.log` · W: `%LOCALAPPDATA%\Clawdmeter\daemon.log`, 512 KB × 3 | Tray: `~/.config/clawdmeter/daemon.log` (previous run kept as `.log.1`) · `--daemon`: stderr (journal) · LaunchAgent: `~/Library/Logs/clawdmeter.log` | No size cap within one run. |
| Token never logged | W: tested | The app does not read tokens directly | |
| Flashing | `flash.sh` / `flash-mac.sh`: `pio run -t upload` by env name | Flash page: release image per board, chip and flash-size detection (`clawdmeter-flash`); `pio run -t upload` for source builds | |

## Tests that moved

The Rust suites cover the retained behavior. Tests of removed implementation
details do not need direct counterparts:

- Ported: interval parsing and `POLL_INTERVAL` (`config.rs`), payload aging
  and heartbeat timing (`payload.rs`, `cadence.rs`), attention file handling
  (`attention.rs`), multi-dir config and the selector (`config.rs`,
  `selector.rs`), payload size trimming (`payload.rs`), transient vs. auth
  failures (`engine.rs`), billing period (`payload.rs`), Windows PnP address
  parsing and the zombie breaker (`clawdmeter-ble`), Windows Run value
  (`autostart.rs`), hook script output (`clawdmeter-app/src/hooks/mod.rs`
  runs the original script, kept verbatim in `hooks/testdata/`, and the Rust
  hook on the same inputs and compares every file; needs `jq`).
- Not ported, because the code they covered is gone: token extraction and
  Keychain hex decoding, HTTP header parsing, Python tray/icon/thread
  plumbing, the venv `requirements-windows.txt` check, the no-WSL source scan.
