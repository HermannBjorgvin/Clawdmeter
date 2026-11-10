# Desktop app

Rust workspace, edition 2021. MSRV 1.88 for the library crates, 1.90 for
`clawdmeter-app`. Run everything from `desktop/`.

## Commands

```bash
cargo fmt --all --check
cargo clippy --workspace --all-targets --locked -- -D warnings
cargo test --workspace --locked
cargo clippy -p clawdmeter-app --target x86_64-pc-windows-gnu -- -D warnings   # Windows cfg check from Linux
cargo run -p clawdmeter-app -- --once     # sanity: real usage (idle source) → payload, no BLE
```

Linux build deps: `.github/actions/linux-desktop-deps/action.yml`.

**Tests must never run a real `systemctl`, `launchctl` or `reg`** against the
developer's machine: use the injectable `autostart::disable_legacy_with`.
GPUI UI tests use `gpui`'s `test-support` (`#[gpui::test]`); flashing hardware
is faked behind `FlashHardware`.

## Crates

- `clawdmeter-core`: pure logic. `config` (the `key = value` file, edited in
  place), `usage` (runs and parses `claude --output-format json -p /usage`: `Current session/week`
  lines, `Spend limit:` lines, the overage header, reset dates with an
  optional year; fixtures in `tests/fixtures/` are real or derived from the
  CLI source), `probe` / `oauth` (usage sources), `cadence` (when to poll),
  `backoff`, `selector` (which account is shown), `events` (tails
  `events.log`), `attention`, `payload`, `buttons` (device button actions,
  their config spelling and `k1`–`k3` codes), `rate`, `engine` (I/O-free state
  machine: `next_write` / `mark_written`), `autostart` (systemd / XDG /
  LaunchAgent / Run key, legacy-daemon detection + `disable_legacy`),
  `launch`, `platform`.
- `clawdmeter-ble`: btleplug link. Adapters come from one `Bluetooth` handle
  per runtime: on Linux every btleplug `Manager` is a D-Bus connection that
  is never closed, so never make one per attempt. On a failed `adapters()`
  a throwaway connection checks whether a new one would fare better; only
  then, and if a retry on the same connection fails too (bluetoothd may
  just have come back), is the manager replaced and the loss counted. The
  watcher reopens on a bump, and dead streams are leaked, not dropped
  (`MessageStream::drop` unwraps a `RemoveMatch`, which panics on a dead
  connection and aborts release builds). Connects only to OS-known
  devices (custom service, or HID + exact name); address cache, REQ
  subscription, zombie-write breaker, BlueZ trust, macOS stale-bond unpair via `blueutil`,
  Windows bonded-address fallback (PnP / `CLAWDMETER_BLE_ADDRESS`).
- `clawdmeter-flash`: downloads the release `manifest.json`, matches boards by
  chip + flash size, writes with espflash.
- `clawdmeter-app`: the `clawdmeter` binary. Tray (`tray/`, ksni on Linux),
  window (`ui/`), `daemon.rs` (drives the engine on its own thread: 0.5 s
  tick, config re-read every 5 s and on `Command::ReloadSettings`),
  `hooks/` (`clawdmeter hook alert|clear|event` and the settings.json
  installer), `ipc.rs` (single-instance lock, status file, command queues,
  window lock), `onboarding.rs`, `boards.rs` (names, buy links,
  illustrations), `chime.rs`.

## Running

- Modes: no args = tray app (logs to `<state>/daemon.log`); `--daemon`
  headless; `--once` (poll + print payload, no BLE); `--window [PAGE]
  [--manifest PATH]`; `--ports`; `--info PORT`; `hook MODE [event]`.
- Env: `CLAWDMETER_LOG`, `CLAWDMETER_NO_TRAY`, `POLL_INTERVAL`,
  `CLAUDE_CONFIG_DIR` (default account), `CLAWDMETER_BLE_ADDRESS`,
  `CLAWDMETER_DECORATIONS`.
- State lives in one dir, `~/.config/clawdmeter/` (Windows:
  `%LOCALAPPDATA%\Clawdmeter\`): `config`, `events.log`, `attention`,
  `clear-<sid>`, `ble-address`, `daemon.log`, `usage-blocks.json`, app
  state and locks.
- Config keys: `config_dirs`, `poll_interval`, `heartbeat_interval`,
  `reset_chime` and `alert_chime` (device chimes; the old `chime` / `beep`
  keys are read when these are absent), `desktop_chime` (the same ding on
  this computer via rodio, ALSA on Linux), `clock`, `liveliness` (1–5,
  default 3: the Device page's Animation silliness slider, sent as `lv`),
  `attention_anim` (`off`/`10s`/`30s`/`2m`, default `2m`, sent as `aa`
  seconds), `pace_marker` (default off, sent as `pm`), `button_primary`
  (default `space`), `button_secondary` (default `shift-tab`) and
  `button_pwr` (default `brightness`): `none`, `space`, `shift-tab`, `tab`,
  `enter`, `escape`, `brightness`, a media key (`play-pause`,
  `previous-track`, `next-track`, `volume-down`, `volume-up`, `mute`) or a
  shortcut, modifiers and a key joined by `+` with an optional `hold`
  (`ctrl+shift+m`, `super+alt+k`, `ctrl+space hold`; `tap` marks one
  that would read as a preset, `space tap`). `clawdmeter_core::buttons` has
  the grammar, the key table and the keystroke→HID mapping; sent as
  `k1`/`k2`/`k3`. The Device page's Controls field draws a keycap per button
  the board has, in the order and under the names in `BoardInfo.buttons`
  (checked against the firmware's `caps.cpp`, as is `pwr_button`); a
  keycap opens its editor underneath, which records a shortcut from the
  next key press.
  `theme` (`system`, the
  default, or `solarized`: the OS-following themes in `themes::FOLLOWING`; a
  preset id from `themes::PRESETS`; or `custom`; the old default
  `clawd` and unknown values read as `system`) and `color_<token>`
  (`rrggbb`, no `#`; the custom theme's colours, all eleven written once it
  has been chosen, which is how it knows not to start from the current theme
  again); the chosen palette's
  eleven colours are sent as `th` (`Settings::colors`; System resolves to
  Dark or Light, Solarized to Solarized Dark or Light, by the OS appearance,
  `platform::AppearanceWatch`: the xdg-desktop-portal `color-scheme` over
  D-Bus, macOS `AppleInterfaceStyle`,
  Windows `AppsUseLightTheme`; portal "no preference" is GNOME's light
  default, unreadable = Dark. A background thread keeps it (Linux: one read,
  then the portal's `SettingChanged` signal; macOS polls every 10 s, Windows
  every 2 s), so neither the daemon's runtime nor the UI thread blocks; the
  window watches only while the Device page shows; the
  daemon hands changes to `Engine::set_appearance`, which sends them like any
  device setting), `claude_path`,
  `release_repo`, `update_check`, `onboarding_done`, `alert_on` (alert kinds
  the daemon lets through: the hook records `k` from `hook_event_name` /
  `notification_type` and `engine.on_attention` drops the rest; default all
  but `idle`).
- Launch paths (`clawdmeter-core::launch`): hooks and Start at login write a
  stable path (`$APPIMAGE`, the Nix profile link, else `current_exe`). Under
  an AppImage, hooks run a copy extracted to `<state>/appimage/` (renewed when
  the AppImage changes) so hook events don't FUSE-mount it; Start at login
  keeps `$APPIMAGE`. Installs refuse paths that won't survive (macOS disk
  image or AppTranslocation, Windows temp/zip folders), and existing entries
  pointing there show as stale.

## Usage sources

Per account (`probe.rs`, `oauth.rs`):

- **Probe**, only while the account's 5-hour window is known to be running:
  `claude -p --model haiku --output-format stream-json --verbose
  --no-session-persistence --strict-mcp-config --tools ""`, hooks and auto
  memory off, cwd = state dir. It reads the `rate_limit_event` headers.
- **Idle source** otherwise, and on a probe miss unless backing off:
  `GET https://api.anthropic.com/api/oauth/usage` with the account's OAuth
  token, read-only, User-Agent `clawdmeter/<version>`. Token lookup: on macOS
  the Keychain first (`Claude Code-credentials`, plus
  `-<sha256(config dir)[..8]>` for a non-default dir, as Claude Code names
  it), then `<config dir>/.credentials.json`; the first unexpired token wins.
- **Clawdmeter never refreshes the token.** On an expired or rejected token,
  or none readable, it falls back to `claude -p /usage` (Claude Code
  refreshes its own token, and that doesn't start a window), under the same
  backoff.
- **The probe must never start a window** (owner's rule). `choose_source`
  allows it only when the last known five-hour reset is more than 5 min away,
  or a prompt/tool event from that account came after it, within 5 h
  (events.log carries the config dir, from the hook input's
  `transcript_path`). A window that ends with no activity since becomes
  "not started" with no call. Anything uncertain (first run, unattributed
  activity, near a reset) uses the idle source, which sends no message.
  Spend-capped accounts always use the idle source, and so do accounts with
  no 5-hour limit.
- **No 5-hour limit** (e.g. verified Ultra, formerly Max 20x): the endpoint
  answers `"five_hour": null`, where an account with the limit and no window
  running gets `{"utilization": 0, "resets_at": null}` (`usage::SessionLimit`).
  Only the endpoint can tell: a probe answer without `five_hour` makes the
  same poll ask the endpoint, never `/usage` (while it's backing off, has no
  readable or unexpired token, or misses, the answer is kept as not started,
  so the probe waits for the next prompt; any miss, a refused token included,
  still backs the account off), and `/usage` drops the session line when no window runs,
  so it leaves `Unknown` and `AccountState::record` keeps the last answer. A
  missing `five_hour` key is `Unknown` too; `record` makes a reading with a
  session window `Applies` (the parsers leave it). An `Absent` answer while
  the last reading's window still runs is taken as `Applies` once: the next
  answer confirms it, so verifying an account costs one poll. The payload is
  `acct:"week"` with `s`/`sr` left "not started" for older firmware; the
  Overview, tray and device drop the session meter, `session_pct()` is the
  week (the selector follows it; before any rise it compares it with other
  accounts' session %, as it does spend %), and the weekly reset wakes the
  account.
  When `meter_kind()` changes (session, week, spend) the selector forgets
  that account's last value, and the rate tracker starts over if it's the
  shown one, so the jump is never a rise.
  The plan name (`planDisplayName`, `rate_limit_tier`) isn't used: an
  unverified Ultra account still has a temporary 5-hour limit.
- `claude` is found via `claude_path` → `PATH` / install dirs → the newest
  Claude-desktop-bundled build (`<userData>/claude-code/<version>/<hash>/claude`;
  verified on Linux, the macOS/Windows paths come from the desktop app's code
  and Electron's userData locations). With no `claude` there's no probe: the
  endpoint is polled every 12 min, window or not.

## Cadence and backoff

- With a window known to be running: a prompt on the shown account polls now,
  then every 3 min while working; ~10 s after a Stop; 5 min with no hooks, or
  between a Stop and 10 quiet min; 15 min after that. Other accounts every
  10 min (`cadence.rs`).
- With no window known (or before the first reading, or without `claude`):
  the idle source every 12 min, not sped up by prompts; a known reset wakes
  the account once.
- No account is polled twice within 2 min, whatever asked (prompt, Stop,
  device REQ, tray "Refresh now").
- Backoff (`backoff.rs`, idle source only): a 429 parks the account for
  exactly its `retry-after`; other transient misses (no 429 header, offline,
  `/usage` printing no `Current …` lines, timeouts) park it 10 → 20 → 30 min,
  reset when the idle source answers. Nothing bypasses it (Refresh now, device
  REQ and prompts included); the probe is a separate path and stays allowed.
  Its answers don't end a pause that's still running; once it has run out
  they clear its strikes, unless the endpoint missed again in that poll.
- Calls during a block make the server lengthen it (589 → 1353 → 2721 →
  3600 s seen), so blocks are saved to `<state>/usage-blocks.json` and
  restored at start-up (`--once` honours them too). Only 429-type misses
  persist (`Backoff::throttled`), until they run out or the idle source
  answers; probe misses and answers don't touch them. The Overview and
  Accounts pages say "Usage paused by Anthropic until HH:MM"; a parked shown
  account keeps its last values.
- Heartbeat replays (`rp:1`) every `heartbeat_interval` (60 s) keep the
  firmware's 90 s `DATA_FRESH_MS` open and stop after 3 consecutive misses,
  so the device shows stale. **A failed poll is never a zero.**

## Window and UI

- **One window, one process.** GPUI 0.2.2 ends its event loop when the last
  window closes on Linux/Windows, so the tray process never runs GPUI: the
  window is a `clawdmeter --window [PAGE]` child process.
- `ui/app.rs` is the root: a sidebar (Overview, Device, Flash, Accounts,
  Hooks, Settings) or the setup wizard (`ui/setup.rs`) filling the window.
  Pages are `crate::Page`; `Page::parse` also accepts the old
  `background`/`flasher`/`status`/`alerts` names.
- The window saves settings through `ui::settings::write_settings`, which
  also queues `settings-changed` on `<state>/app-commands`; the tray (it
  takes the queue every 200 ms) forwards it as `Command::ReloadSettings`, so
  a device setting goes out within about a second. `Engine::next_write` sends
  a changed `lv`/`aa`/`pm`/`k1`–`k3`/`th` or clock format (`tf`, off
  included) at once, unless the numbers are stale (then it waits for the
  next good poll).
  The 5 s re-read stays for hand edits and `--daemon` without a tray.
- Only one window process runs (`<state>/window.lock`). The tray, or a second
  `--window` launch, appends `show <page>` to `<state>/window-commands`; the
  window switches page and raises itself. Child views ask for another page by
  emitting `ui::Navigate`.
- Nothing competes for the serial port: the flasher (`ui/flasher.rs`) touches
  USB only while on screen (`set_active`), the Device page probes only while
  shown, and the sidebar locks while a board is being written.
- The wizard reuses the flasher (`FlashMode::Setup`: no step header,
  up-to-date boards can skip) and the Hooks page
  (`SettingsView::show(Page::Hooks, embedded)`). First run = no
  `onboarding_done` and no cached BLE address; the tray then opens the window
  on `setup`.
- Look: neutral greys in the Claude desktop app's family (`ui/theme.rs`);
  Clawd orange only for Clawd and primary actions; notices are neutral unless
  they report a problem.
- Window frames (`ui/decorations.rs`): native wherever the platform draws one
  (macOS/Windows, X11 WM, Wayland xdg-decoration on KDE/Sway/…). On a Wayland
  compositor without xdg-decoration (GNOME) the app asks for client
  decorations and `ui::chrome` draws an Adwaita-style header bar. gpui 0.2.2
  reports "server" even with no decoration manager, so
  `decorations::wanted()` probes the Wayland registry itself.
  `CLAWDMETER_DECORATIONS=client|server` overrides.
- Board art is bundled by `ui::Assets` and drawn with
  `img(board.illustration())`. Use `img()`, not `svg()`: `svg()` renders a
  monochrome mask, `img()` renders the SVG through resvg in full colour.
  `boards::tests` checks the board table and asset registration. New boards:
  `porting-a-board` skill.
- **GPUI gotcha:** don't `observe_window_appearance` on X11 (0.2.2 redraws
  inside the portal callback and panics); the root polls the appearance.
- UI work without hardware: `cargo run -p clawdmeter-app --example demo --
  --page device` opens the window with a fake board, live status and local
  release. Point `HOME`/`XDG_CACHE_HOME` at a scratch dir (it writes there).
  Screenshots: `capturing-desktop-screenshots` skill.
