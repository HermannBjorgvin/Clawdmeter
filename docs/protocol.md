# BLE protocol

Usage readings and hook events reach the app independently. The app selects
an account, builds a JSON payload, and writes it to the device over Bluetooth
LE. The firmware updates the meters and passes activity to Clawd's director.
Button presses use the separate HID service: a keyboard (report ID 1, the
usual 8-byte boot-compatible report, key usages up to F24) and a consumer
control (report ID 2, one 16-bit usage, for media keys). Hosts that bonded
to firmware with an older report map get a Service Changed indication when
they next connect (`GATT_REVISION` in `firmware/src/ble.cpp`); a host that
keeps its cached map needs the board forgotten and paired again.

## Service and characteristics

The device advertises a custom GATT service next to the standard HID keyboard service:

|                                | UUID                                   |
| ------------------------------ | -------------------------------------- |
| **Data service**               | `4c41555a-4465-7669-6365-000000000001` |
| RX characteristic (write)      | `4c41555a-4465-7669-6365-000000000002` |
| TX characteristic (notify)     | `4c41555a-4465-7669-6365-000000000003` |
| REQ characteristic (notify)    | `4c41555a-4465-7669-6365-000000000004` |
| INFO characteristic (read)     | `4c41555a-4465-7669-6365-000000000005` |
| OTA characteristic (write)     | `4c41555a-4465-7669-6365-000000000006` |
| **HID service**                | `00001812-0000-1000-8000-00805f9b34fb` |

The host writes one complete JSON object to RX per write. The app limits
payloads to 500 bytes; the firmware has a 512-byte receive buffer, including
the terminating null byte. The device notifies `0x01` on REQ when it has no
data yet, asking for a poll.
It sends `{"dismiss":true}` on TX when a tap or the device's 2-minute timeout
ends an alert, so the app clears its corresponding alert too. The app expires
an alert nothing cleared after the same 2 minutes (a test alert after 30
seconds) and sends `x` if the device still shows it.

```json
{"s":45,"sr":120,"w":28,"wr":7200,"st":"allowed","acct":"pro","ok":true,"a":2,"e":"p","es":14,"n":1}
```

A Codex account (a $100 Pro plan: weekly window only, reset credits):

```json
{"s":0,"sr":-1,"w":34,"wr":7020,"st":"allowed","acct":"week","pv":"codex","rc":2,"rx":["Dec 1","Dec 13"],"ok":true}
```

| Field | Meaning |
| --- | --- |
| `s`, `w` | Session and weekly % used (`s` is the spend % for `ent`). |
| `sr`, `wr` | Minutes until each resets; `-1` = window not started / unknown. |
| `wf` | Fable's weekly % of its own limit (0–100, above 100 past it). The device draws it as a violet strip from the left end of the bar that shows the week, `wf` × 50 % of the bar wide (never past the weekly fill; Fable usage is part of the weekly usage, and `wf` and `w` are rounded apart, so when `wf`/2 is within 1.5 points of `w`, or past it, the strip runs to the fill's end: `min(wf, 100) × 50 + 150 ≥ w × 100`, the same integer rule in the firmware's `fable_reaches_fill` and the app's `usage::fable_reaches_week`; the device also hides it while the fill is narrower than its two rounded ends), over the fill and under the pace line, with a straight right end (the fill's rounded corner where it reaches the fill's end): Fable's weekly limit is half the overall weekly limit, so a used-up Fable limit reaches the halfway point while the fill keeps showing `w`. The 50 % is one constant on each side, `FABLE_WEEKLY_SHARE_PCT` (`firmware/src/fable.h`, `clawdmeter_core::usage`); the endpoint's `weekly_scoped_shares` gives the same figure. The app reads Fable from the usage endpoint's `limits[]` row `{"kind":"weekly_scoped","scope":{"model":{"display_name":"Fable"}}}`, the `/usage` line `Current week (Fable): …`, or the probe's `seven_day_overage_included` window, which Claude Code calls the "Fable limit"; a probe answer without it keeps the last known value (back at 0 % once its reset passes). The name matches ignoring case; other scoped models (Opus, Sonnet) aren't sent. Absent (or negative) = unknown or no Fable limit: no strip, never a 0 % one. Not sent with `acct:"ent"` (no weekly bar) or with `pv:"codex"` (Fable is Claude's; the firmware ignores one there too). Its colour is `fc`. 1-bit panels (`BoardCaps.mono_panel`) don't draw it: it would print as the same ink as the fill. The rate math and reset detector never read it; older firmware ignores it. |
| `st` | `allowed`, `limited` or `overage` (stored, not shown). |
| `acct` | `pro`, `week` or `ent`. `week` is an account the usage endpoint reports with no 5-hour window (`"five_hour": null`), such as a verified Ultra account (formerly Max 20x): `s`/`sr` stay `0`/`-1`, so older firmware draws what it always did (the session not started), and newer firmware takes `w`/`wr` as its main meter, shown alone in the first card, with the reset chime and pace marker following the week. The usage-rate mood stays tuned for the 5-hour window (whole weekly points in a quarter hour barely move), so for these accounts Clawd's mood comes from hook activity (`a`). When the kind changes between `pro`, `week` and `ent`, the device starts its rate history over instead of reading the jump as a reset. `ent` is a spend-capped account: `s` is then the spend %, and `tp` (% of the billing period elapsed), `pd` (period length in days) and `rd` (reset date, e.g. `"Nov 1"`) fill the second card. |
| `pv` | Whose usage the numbers are: `codex` for an OpenAI Codex account (a trial); absent for Claude. The usage fields keep their meanings: a weekly-only Codex plan (the $100 Pro plan today) is `acct:"week"`, one with a 5-hour window the normal `pro` shape. The firmware then draws its Codex screen: the first card's pill reads `Codex` (the 5-hour window when there is one, else the week), the bars are white (`th`'s text colour) with the pace marker in the secondary text colour, and with `rc` on a weekly-only plan the second card is the Resets card. The Codex screen never draws the Fable strip: `wf` and `fc` aren't sent with `pv`, and the firmware hides the strip there even after a Claude payload drew one. Codex numbers never reach Clawd: the device treats them as a replay for the rate math, the reset chime and his reaction to a rising meter, and keeps its last Claude "window not started" state; hook fields in the same write still count. Older firmware ignores `pv` and draws the numbers as a Claude account's. |
| `rc`, `rx` | A Codex account's reset credits: `rc` credits still held, `rx` each held credit's expiry date in the computer's local time, formatted like `rd` (`["Dec 1","Dec 13"]`), soonest first, at most 10. Both are absent when unknown, never `0` (or `[]`) for "unknown". The device's Resets card shows `rc` in a count pill and the `rx` dates as chips, soonest first, as many as the panel's grid holds (six at most; with more credits than cells, one fewer date and "+N more"), "No resets available" at `rc:0` and "Expiry dates unknown" when `rx` is absent. Without `rc` a weekly-only Codex plan's card sits alone, as `week` does; a plan with a 5-hour window keeps the week in the second card and shows no Resets card. |
| `t`, `tf` | Local wall-clock epoch and 12/24-hour format, when the clock is on. Sent with every data payload and right away when the clock setting changes (turning it off too). |
| `c` | `1` = the reset chime may sound. |
| `ok` | `false` alone = no usable account data; do not treat this as a zero reading. |
| `b`, `m`, `x` | Attention: alert chime, message (ASCII), end the alert. The app sends a fixed line per alert kind in `m`: `Question for you`, `Permission needed!` (also for other notification types), `Waiting for you!` (idle) or `MCP request!`; the test alert sends `Clawdmeter test alert`. The firmware shows whatever `m` it gets. |
| `a`, `e`, `es`, `n` | Fast clock: activity 0–3, latest hook event (`p` prompt, `s` stop, `b` back after a gap) and its sequence number, active sessions. |
| `rp` | `1` = the usage numbers are a replay, not a fresh poll (heartbeats, and writes between polls for an event, `a`/`n` change, alert, setting or a switch to another account's numbers read earlier); the rate math and reset detection skip them. Fast-clock fields in the same write still count: a new `es` fires. |
| `lv` | How lively Clawd is, `1` (No-nonsense) to `5` (All-nonsense); `3` is the default. Sent with every data payload and right away when the setting changes. The device stores the last value in NVS, so it applies from boot before the app connects; absent = keep the stored value. |
| `aa` | How long Clawd points and waves for an alert, in seconds: `120` (the default, the whole alert), `30`, `10`, or `0` = no animation. Only the animation: the alert's message, tap, `x`, timeout and dismiss notify are unchanged. Sent like `lv` and stored the same way (clamped to 0–120); absent = keep the stored value. |
| `pm` | Pace marker: `1` draws a line on the session and weekly bars at the elapsed share of each window (5 hours, 7 days) and colours the fill by pace (green at or behind it, amber ahead, red at 100%); `0` keeps the plain 50/80% colours. A bar with no reset time (`sr`/`wr` = -1) and spend-capped accounts keep the plain colours. Sent like `lv` and stored the same way; absent = keep the stored value. |
| `k1`, `k2`, `k3` | What the primary, secondary and PWR buttons do (a PWR short press; its hold to pair is fixed). An integer: `0` nothing, `1` Space (held while the button is held: voice push to talk), `2` Shift+Tab, `3` Tab, `4` Enter, `5` Esc, `6` cycle brightness; `0x10000 \| hold << 12 \| mods << 8 \| usage` a keyboard shortcut, `usage` a HID keyboard usage `0x04`–`0x73` (A to F24), `mods` the HID modifier bits Ctrl `1`, Shift `2`, Alt `4`, GUI (Cmd/Win/Super) `8`, `hold` `1` = the keys stay down while the button is held, `0` = tapped; `0x20000 \| usage` a HID Consumer usage `1`–`0x3FF` (the app sends Play/Pause `0xCD`, Previous `0xB6`, Next `0xB5`, Volume down `0xEA`, Volume up `0xE9`, Mute `0xE2`; volume repeats while the button is held, the rest tap). Example: Ctrl+Shift+M tapped is `66320` (`0x10310`). Defaults `k1` = `1`, `k2` = `2`, `k3` = `6`; a value outside these fields, or not an integer, is ignored like an absent key (the device keeps its stored action; the app reads an unknown config value as the default). PWR always taps: its press registers on release. On touchless boards a primary press (or a PWR press, where PWR is the only button) first dismisses an alert whatever its action. Sent like `lv` and stored the same way; absent = keep the stored value. Boards ignore the value for a button they don't have; the T4-S3's long BOOT hold, which stands in for a PWR press there, stays brightness. |
| `th` | Palette: 66 hex digits, eleven `rrggbb` colours run together in this order: background, cards, text, secondary text, accent, green, amber, red, bar track, Clawd, face eyes (`firmware/src/theme_palette.h`). The board repaints live and keeps it in NVS; a string of any other length or with a non-hex digit is ignored. The Clawd colour paints the screensaver face and the sprite: at draw time the official art's body orange and the shaded side of his body become that colour and the same shade of it, while his eyes, props and scenery keep their own colours (`theme_clawd_art`; the frames are never changed). The app sends a preset theme's palette (for the System theme, Dark or Light by the computer's appearance) or the user's own colours; the board doesn't know which. Boards with a 1-bit panel (`BoardCaps.mono_panel`, the e-paper board) ignore it and keep the default palette. Absent = keep the stored palette. Always eleven colours: older firmware drops any other length, so colours added later get keys of their own (`fc`). |
| `fc` | The Fable overlay's colour (`wf`): six hex digits, `rrggbb`. Each app theme has its own: for the editor presets, the colour in the slot the scheme's published palette names purple (or mauve, violet), whatever it looks like (Catppuccin Mocha mauve `cba6f7`, Dracula `bd93f9`, Gruvbox `d3869b`, Monokai `ae81ff`, Nord nord15 `b48ead`, One Dark `c678dd`, Solarized violet `6c71c4`, Tokyo Night `9d7cd8`) and, since Anthropic's brand palette has no purple, a violet at the brand accents' saturation and lightness for Dark (`9775c7`) and a deeper one for Light (`644294`); Custom has its own (`color_fable`). Sent with `th` on every data payload but a Codex one (`pv`), and right away when it changes; the board repaints live and keeps it in NVS. Anything but six hex digits is ignored; absent = keep the stored colour, `9775c7` until one arrives. 1-bit panels ignore it. |

## Firmware updates

INFO and OTA let the app install firmware over the same link (`GATT_REVISION`
4 added them). The firmware's half is `firmware/src/ota.cpp` and
`ota_proto.h`, the app's `clawdmeter-ble`'s `ota.rs` and `Updater`.

INFO reads as one JSON object:

```json
{"env":"waveshare_amoled_216","ver":"v1.10.0","rev":"","ota":3342336,"otav":1}
```

`env` and `ver` are what the serial `info` command reports, `ota` is the
size in bytes of the app slot an update goes into (`0`: the board has a
single app slot, as the 4 MB boards do, and updates over USB only), `otav`
the update protocol below (`OTA_PROTO_VERSION`). On every connect the app
records `env` and `ver` for its update nudge. Firmware without INFO predates
Bluetooth updates.

Only the owner, over an encrypted link, may write OTA: the characteristic
requires encryption, and the firmware drops writes from anyone else. They
never claim ownership the way a first RX write does. The owner is the
bonded identity address and pairing is Just Works, and images aren't
signed, so the board doesn't rely on either: every update also needs a tap
or button press on the board itself (step 1).
Frames are binary, integers little-endian, the first byte the op:

| Frame | Layout | Written |
| --- | --- | --- |
| BEGIN | `0x01`, u32 image size, 32-byte SHA-256 of the image, env (ASCII, 1 to 40 bytes) | with response |
| DATA | `0x02`, u32 offset, payload (up to ATT MTU − 3 − 5 bytes) | without response |
| END | `0x03` | with response |
| ABORT | `0x04` | with response |

The board replies on TX with notifications whose first byte is `0x80` or
above, so they never read as the JSON acks TX also carries:

| Reply | Layout | Meaning |
| --- | --- | --- |
| CONFIRM | `0x86`, u8 seconds | BEGIN is valid; the board asks on its screen and waits this long for a tap or button press |
| READY | `0x81`, u16 ATT MTU, u32 window | allowed on the board; at most `window` bytes may be unacknowledged |
| AT | `0x82`, u32 offset | bytes written to flash so far (every 2 KB and at the end; again, at most every 0.5 s, when chunks arrive that are already written, in case the last AT was lost) |
| RESEND | `0x83`, u32 offset | a chunk went missing or the buffer was full: send again from here (once per offset) |
| DONE | `0x84` | image checked, boot slot switched; restarting about a second later |
| ERROR | `0x85`, u8 code | stopped: 1 no update slot, 2 too large, 3 another board's image, 4 bad frame, 5 flash error, 6 checksum mismatch, 7 image rejected, 8 no update running, 9 incomplete, 10 out of memory, 11 no data for 30 s, 12 not allowed on the board in time, 13 busy (another connection's update or question, or within 30 s of an unanswered one) |

The exchange:

1. The app writes BEGIN with an application image (the merged release image
   from its first app partition on, `clawdmeter_flash::device::app_image`).
   The board refuses another env's image or one larger than its slot, then
   answers CONFIRM and shows "Update firmware?" with a 30-second countdown.
   Nothing is written until someone taps the screen or presses a button
   twice in quick succession; then the board starts a sequential write into
   the spare slot and answers READY. The question belongs to the connection
   that asked: another one gets ERROR 13, and a repeat BEGIN from the same
   one keeps the countdown. Taps and presses in its first second don't
   count; after that every button press goes to it and does nothing else
   (no keystroke reaches the computer). With no answer in time it answers
   ERROR 12, and for 30 s after an unanswered or cancelled question every
   BEGIN gets ERROR 13, so a question can't be kept up waiting for a press
   out of habit. The serial `ota_allow` command allows it too (USB access can
   flash the board anyway). This logic is `OtaGate` in `ota_proto.h`, covered
   by `test_ota`.
2. The app streams DATA frames while fewer than `window` bytes are
   unacknowledged (written without response; after a write that stalls with
   the link up, as macOS's queue sometimes does, the rest go with response,
   which the board takes the same way). The BLE callback only appends in-order chunks to a
   receive buffer (16 KB in PSRAM, else 8 KB); the loop task writes it to
   flash, erasing each sector as the data reaches it, and hashes it. A chunk
   past the expected offset means one was lost: the board drops it and asks
   for a RESEND, and the app goes back there. Chunks before the expected
   offset are duplicates and are dropped. With no reply for 1.5 s the app
   goes back to the last AT (the lost chunk may be the last one sent).
3. Once AT reaches the image size, the app writes END. The board compares
   the SHA-256 with BEGIN's, has ESP-IDF check the image as the bootloader
   will, switches the boot slot, answers DONE, stops advertising (so nothing
   reconnects to the old firmware in its last second), drops its links and
   restarts.
4. The app waits for the board to reconnect and reads INFO. The new version
   counts once a payload written to it has held for 20 s (or the link alone
   for 30 s); the old version 20 s or more after DONE means the board went
   back to its previous firmware; no INFO for 2 minutes is a failure.

The app writes no usage payloads while an update streams. A new BEGIN
starts over; ABORT (the app sends one on cancel and on any failure with the
link up), a dropped link or 30 s without data drops the update, and the
board keeps running the firmware it has.

A new image boots in the bootloader's pending state (`verifyRollbackLater`
returns true in `ota.cpp`, so Arduino doesn't confirm it at startup). The
firmware confirms it once the owner's computer is connected over an
encrypted link and a payload has parsed, or a minute after that link came up
with none, or when another update is allowed on it (ESP-IDF won't write the
other slot from an unconfirmed image); a reset before then (a crash, an image that crashes on its first
payload, the power button) makes the bootloader boot the previous firmware.
A USB flash leaves no pending state.
