# Multi-screen Usage Carousel Design

## Goal

Turn the USB-powered ESP32-2432S024C into a compact 240 x 320 desk dashboard
that cycles through Claude Code usage, Codex usage, local task activity, and the
existing robot animation. The user can advance screens manually with one touch,
while an automatic carousel keeps the display useful when left unattended.

This design also corrects the current small-display layout: the 80 x 80 header
logo overlaps the first metric panel, the battery icon appears on hardware with
no battery, and the two usage panels consume too much vertical space.

## Approved navigation

The dashboard has four screens in this fixed order:

1. Claude
2. Codex
3. Activity
4. Robot

The navigation contract is:

- a short touch anywhere advances to the next screen;
- after the Robot screen, the next touch returns to Claude;
- without interaction, the screen advances every 12 seconds;
- a manual touch pauses automatic advancement for 30 seconds;
- three metric screens show four small page indicators at the bottom, with the
  active screen highlighted;
- the Robot screen keeps the existing animation and shows connection status and
  the age of the last successful data update;
- the existing BOOT-button behavior remains unchanged.

A simple click is preferred over swipe gestures because it is easier to discover
and more reliable on the board's CST816/CST820-compatible touch controller.

## Screen contents

### Claude screen

Show the two subscription-limit values already collected by the Windows daemon:

- current or five-hour utilization, percentage and reset time;
- weekly or seven-day utilization, percentage and reset time;
- concise connectivity/freshness text at the bottom.

Labels sent by the daemon determine the window names so the display does not
assume that every account always exposes the same rate-limit windows.

### Codex screen

Show the rate-limit windows exposed by the latest local Codex session event.
The current account may expose only a weekly window, so the layout must tolerate
one or two limit cards without inventing a missing five-hour value.

When only one limit window is available, the second card shows local daily Codex
activity, such as tokens used today. Small secondary text may show the plan or
model, but it must not displace the primary utilization value.

The Windows daemon reads the newest `token_count` event under
`%USERPROFILE%\.codex\sessions\**\*.jsonl`. This is local application state, not
a supported public API. Parsing therefore uses optional fields, validates window
durations, and reports data as unavailable if the schema no longer matches.

### Activity screen

Keep Claude Code and Codex states separate so different concepts are not merged:

- Claude Code: open interactive sessions, busy sessions, and idle sessions;
- Codex: unread tasks and, only when reliably detected, currently running tasks;
- time of the last successful local-state scan.

Claude Code session files under `%USERPROFILE%\.claude\sessions\*.json` expose
explicit `status` values such as `busy`, `idle`, and `shell`. An idle session may
be labelled `Waiting` in the compact UI, while the implementation documentation
must clarify that this means the CLI is idle and is not proof of a particular
unanswered prompt.

Codex unread task IDs are available in `.codex-global-state.json`. They are
labelled `Unread`, not `Needs reply`. A Codex running count is displayed only if
a stable local signal is found during implementation; otherwise the field is
omitted rather than inferred from historical session files.

### Robot screen

Retain the existing animated mascot. Add only compact operational text:

- transport state (`USB connected` or `No data`);
- age of the last valid update;
- optional current action text already used by the animation.

The Robot screen is part of the carousel, not a special toggle outside the page
sequence.

## 240 x 320 visual layout

The ESP32-2432S024C uses a board-specific small-display profile:

- remove the battery object entirely when `board_caps().has_battery` is false;
- scale the header logo from 80 x 80 to approximately 44-48 pixels;
- reserve a header area that cannot overlap the first metric card;
- reduce usage-card height from 100 pixels to approximately 86-92 pixels;
- reduce the large percentage font from 28 pixels to approximately 24 pixels;
- keep reset text, pills, and status text at 12-14 pixels;
- reserve the bottom strip for freshness text and page indicators;
- use provider-specific titles (`Claude`, `Codex`, `Activity`) instead of the
  generic `Usage` title.

Exact dimensions may move by a few pixels during physical validation, but both
metric cards, reset text, status text, and page indicators must remain fully
visible without overlap.

## Data and transport architecture

The existing Windows USB serial daemon remains the single collector. The ESP32
does not read files, contact Anthropic/OpenAI services, or need Wi-Fi credentials.

The daemon performs three read-only collectors on each update cycle:

1. Claude subscription collector: existing Anthropic utilization and reset data.
2. Codex local collector: newest valid local rate-limit event plus daily token
   aggregation when available.
3. Activity collector: Claude Code live session states and Codex unread state.

The serial JSON message is extended in a backward-compatible way. Existing
Claude fields remain accepted. New nested objects carry Codex and Activity data,
along with a message timestamp or freshness value. Missing optional fields render
as unavailable and do not clear the last valid values for unrelated providers.

The firmware stores a compact dashboard state and updates only visible labels
when data changes. Screen changes do not request fresh data; the carousel always
uses the most recently validated payload.

## Timing and state transitions

- Start on the Claude screen after the first valid payload.
- Before any valid payload, the Robot screen may remain visible with `No data`.
- Automatic carousel interval: 12 seconds.
- Manual-interaction holdoff: 30 seconds from the most recent short touch.
- Receiving new serial data does not reset the carousel timer.
- A screen transition resets only view-specific animation state.
- Wraparound order is deterministic: Claude -> Codex -> Activity -> Robot ->
  Claude.

## Error handling and privacy

- Local collectors read aggregate fields only and never transmit prompt text,
  response text, file paths, or task titles to the ESP32.
- Malformed or partially written JSONL lines are skipped.
- Missing Claude/Codex state directories produce an unavailable provider state,
  not a daemon crash.
- Unknown Claude session statuses count as open but not busy or waiting.
- Codex history files must not be counted as currently open tasks.
- An unread count of zero is valid and distinct from unavailable.
- Serial payload parsing remains bounded for the ESP32's 4 MB flash/no-PSRAM
  target.

## Testing and physical verification

Implementation follows test-driven development where behavior is host-testable:

1. Add failing tests for small-display geometry, no-battery visibility, page
   ordering, carousel timing, pause timing, and wraparound.
2. Add failing daemon tests using sanitized Claude session, Codex session, and
   Codex global-state fixtures.
3. Add payload compatibility tests for old Claude-only messages, complete new
   messages, missing Codex windows, zero unread tasks, and malformed local files.
4. Build the `esp32_2432s024c` PlatformIO environment.
5. Upload through COM3 and confirm serial initialization and payload ACK.
6. Physically verify that all four screens fit, colors and orientation remain
   correct, the battery icon is absent, touch advances one page, 12-second
   rotation works, and a touch pauses rotation for 30 seconds.
7. Compare displayed aggregate counts with the current local Claude Code and
   Codex state before claiming success.

## Acceptance criteria

- No header image or text overlaps either metric card.
- No battery indicator is created or shown on ESP32-2432S024C.
- Claude, Codex, Activity, and Robot screens are all reachable by touch.
- Screens advance automatically every 12 seconds without interaction.
- A manual touch advances once and pauses automatic rotation for 30 seconds.
- Claude usage continues to update through the existing serial transport.
- Codex displays every rate-limit window actually present without assuming a
  missing window.
- Activity distinguishes Claude `Waiting` from Codex `Unread`.
- No prompt, response, project path, or task title is sent to the ESP32.
- Host tests, PlatformIO build, COM3 upload, serial ACK, and physical visual/touch
  validation all pass.

## Out of scope

- Scraping the ChatGPT website for image, voice, upload, or general consumer
  message limits.
- Treating OpenAI API organization usage as ChatGPT subscription usage.
- Wi-Fi transport, Bluetooth transport, OTA updates, or cloud relay services.
- Swipe navigation, on-device settings menus, editable carousel timing, or
  persistent per-screen preferences in this iteration.
- Inferring that every unread Codex task requires a user response.
