# What this fork changes

A fork of [HermannBjorgvin/Clawdmeter](https://github.com/HermannBjorgvin/Clawdmeter)
that turns the device from a passive usage meter into a desk-side Claude Code
**pager** — typed sound/screen alerts driven by Claude Code hooks, meeting
reminders, a live activity indicator, and a runtime-switchable RU/EN
interface. Verified on the Waveshare ESP32-S3-Touch-AMOLED-2.16 with the
macOS daemon.

## Localization

- Every firmware string lives in a string table
  (`firmware/src/lang.{h,cpp}`) with Russian and English sets, including all
  ~90 whimsical status verbs ("Clauding…", "Combobulating…").
- The language switches via `lang = ru|en` in the daemon config
  (`~/.config/claude-usage-monitor/config`); the choice persists on the
  device (NVS), so even the pairing screen speaks the right language from
  the next boot. Without the option the firmware behaves exactly like
  upstream (English).
- Cyrillic in the brand fonts via LVGL fallbacks: Golos Text → Styrene,
  PT Serif → Tiempos; the mono font regenerated from DejaVu Sans Mono.

## Claude Code alerts

- Claude Code hooks (`Notification` / `Stop` / `UserPromptSubmit` /
  `PostToolUse` → `tools/claude-attention-hook.sh`) classify events, the
  daemon forwards them, and the firmware shows a typed screen with its own
  melody (sine synthesis through the ES8311, no PCM clips):
  - **"Awaiting your reply"** — the session is blocked on your input;
  - **"Permission needed"** — a tool-permission prompt;
  - **"Done!"** — a turn finished (turns shorter than 45 s stay silent);
  - **"Limit near!"** — crossing 80 % / 95 % of the session window;
  - **"Limits refreshed!"** — the session window reset (a wink).
- The alert header shows the project name (the git-root basename), wrapped
  over up to two lines. Start typing on the Mac and the screen dismisses
  itself.
- False positives are filtered out: background tasks and parallel agents
  are verified for real — a shell task by the output file its shell holds
  open (lsof), an agent by whether it has reported back to its session
  (the harness's own completion notification), with transcript freshness
  bounding both so a killed one can't pin the session as busy. System
  notifications don't count as user input, autonomous sessions never ring.

## Calendar

- Meeting reminders **15 and 5 minutes** ahead (configurable via
  `cal_remind`) and a separate **"Meeting started!"** event (yellow, within
  a two-minute window after the start — a daemon that overslept stays
  silent).
- The source is the `cal_ics_url` option: an https ICS feed (a published
  Outlook/Google calendar), a local `.ics`, or `eventkit` — the macOS
  system calendar read via the signed Swift helper `daemon/calnext`
  (plain python gets silently refused EventKit access; build with
  `daemon/build_calnext.sh`).
- The ICS path expands recurring events (RRULE, Windows TZIDs) and drops
  all-day and cancelled entries; only the closest active threshold fires,
  and a reminder is marked sent only after a successful BLE write.

## Limits

- Usage comes from the read-only `GET /api/oauth/usage` endpoint (consumes
  nothing); the upstream probe request stays as a fallback and for
  Enterprise accounts.
- A **third gauge** — the model-scoped weekly limit (`weekly_scoped`, e.g.
  Fable): a compact pill in the Weekly panel colored by thresholds.
- Next to the "resets in …" countdown — the wall-clock reset time:
  "Resets in 4h 20m (Sun 00:20)".
- On poll failures the device honestly names the cause ("Update token",
  "No network"…) instead of rendering stale numbers; the poll interval
  backs off exponentially. The daemon deliberately never refreshes the
  token — refresh-token rotation could log Claude Code out.
- **Credentials that outlive the app.** Claude Code refreshes its 8-hour
  OAuth token only while it is running, so any longer gap used to leave the
  display stuck on "Update token". The daemon now prefers a long-lived
  token from `claude setup-token` (Keychain item `Clawdmeter-token`,
  overridable via `token_keychain_service`) and falls back to Claude Code's
  own entry when it is absent or rejected. It also skips a locally-expired
  token instead of spending a guaranteed 401, and cuts the backoff short
  the moment the stored credentials change — so the display recovers within
  seconds of Claude Code refreshing, not up to 10 minutes later.

## Live activity indicator

- An active-session counter (heartbeat hooks) — "·N" in the corner and a
  "Resting" status with a frozen spinner when Claude is idle.
- Splash animations follow the number of working sessions: 1–2 — work
  animations, 3 — "dance sway dj", 4 — "dance bounce dj", 5+ — "dance
  djmix" at double tempo.

## Misc

- Numeric battery percent next to the icon, visible while charging too.
- Screen off after 5 minutes of Claude inactivity (auto-wake on activity
  and events).
- Serial commands `usage` / `splash` switch screens without buttons (for
  QA screenshots via `screenshot.sh`).
- Upstream bugfix: ArduinoJson's `doc["x"] | false` silently drops integer
  flags — replaced with `.as<bool>()`.
