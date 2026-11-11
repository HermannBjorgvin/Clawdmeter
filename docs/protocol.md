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

| Field | Meaning |
| --- | --- |
| `s`, `w` | Session and weekly % used (`s` is the spend % for `ent`). |
| `sr`, `wr` | Minutes until each resets; `-1` = window not started / unknown. |
| `st` | `allowed`, `limited` or `overage` (stored, not shown). |
| `acct` | `pro`, `week` or `ent`. `week` is an account the usage endpoint reports with no 5-hour window (`"five_hour": null`), such as a verified Ultra account (formerly Max 20x): `s`/`sr` stay `0`/`-1`, so older firmware draws what it always did (the session not started), and newer firmware takes `w`/`wr` as its main meter, shown alone in the first card, with the reset chime and pace marker following the week. The usage-rate mood stays tuned for the 5-hour window (whole weekly points in a quarter hour barely move), so for these accounts Clawd's mood comes from hook activity (`a`). When the kind changes between `pro`, `week` and `ent`, the device starts its rate history over instead of reading the jump as a reset. `ent` is a spend-capped account: `s` is then the spend %, and `tp` (% of the billing period elapsed), `pd` (period length in days) and `rd` (reset date, e.g. `"Nov 1"`) fill the second card. |
| `t`, `tf` | Local wall-clock epoch and 12/24-hour format, when the clock is on. Sent with every data payload and right away when the clock setting changes (turning it off too). |
| `c` | `1` = the reset chime may sound. |
| `ok` | `false` alone = no usable account data; do not treat this as a zero reading. |
| `b`, `m`, `x` | Attention: alert chime, message (ASCII), end the alert. The app sends a fixed line per alert kind in `m`: `Question for you`, `Permission needed!` (also for other notification types), `Waiting for you!` (idle) or `MCP request!`; the test alert sends `Clawdmeter test alert`. The firmware shows whatever `m` it gets. |
| `a`, `e`, `es`, `n` | Fast clock: activity 0–3, latest hook event (`p` prompt, `s` stop, `b` back after a gap) and its sequence number, active sessions. |
| `rp` | `1` = the usage numbers are a replay, not a fresh poll (heartbeats, and writes between polls for an event, `a`/`n` change, alert or setting); the rate math and reset detection skip them. Fast-clock fields in the same write still count: a new `es` fires. |
| `lv` | How lively Clawd is, `1` (No-nonsense) to `5` (All-nonsense); `3` is the default. Sent with every data payload and right away when the setting changes. The device stores the last value in NVS, so it applies from boot before the app connects; absent = keep the stored value. |
| `aa` | How long Clawd points and waves for an alert, in seconds: `120` (the default, the whole alert), `30`, `10`, or `0` = no animation. Only the animation: the alert's message, tap, `x`, timeout and dismiss notify are unchanged. Sent like `lv` and stored the same way (clamped to 0–120); absent = keep the stored value. |
| `pm` | Pace marker: `1` draws a line on the session and weekly bars at the elapsed share of each window (5 hours, 7 days) and colours the fill by pace (green at or behind it, amber ahead, red at 100%); `0` keeps the plain 50/80% colours. A bar with no reset time (`sr`/`wr` = -1) and spend-capped accounts keep the plain colours. Sent like `lv` and stored the same way; absent = keep the stored value. |
| `k1`, `k2`, `k3` | What the primary, secondary and PWR buttons do (a PWR short press; its hold to pair is fixed). An integer: `0` nothing, `1` Space (held while the button is held: voice push to talk), `2` Shift+Tab, `3` Tab, `4` Enter, `5` Esc, `6` cycle brightness; `0x10000 \| hold << 12 \| mods << 8 \| usage` a keyboard shortcut, `usage` a HID keyboard usage `0x04`–`0x73` (A to F24), `mods` the HID modifier bits Ctrl `1`, Shift `2`, Alt `4`, GUI (Cmd/Win/Super) `8`, `hold` `1` = the keys stay down while the button is held, `0` = tapped; `0x20000 \| usage` a HID Consumer usage `1`–`0x3FF` (the app sends Play/Pause `0xCD`, Previous `0xB6`, Next `0xB5`, Volume down `0xEA`, Volume up `0xE9`, Mute `0xE2`; volume repeats while the button is held, the rest tap). Example: Ctrl+Shift+M tapped is `66320` (`0x10310`). Defaults `k1` = `1`, `k2` = `2`, `k3` = `6`; a value outside these fields, or not an integer, is ignored like an absent key (the device keeps its stored action; the app reads an unknown config value as the default). PWR always taps: its press registers on release. On touchless boards a primary press (or a PWR press, where PWR is the only button) first dismisses an alert whatever its action. Sent like `lv` and stored the same way; absent = keep the stored value. Boards ignore the value for a button they don't have; the T4-S3's long BOOT hold, which stands in for a PWR press there, stays brightness. |
| `th` | Palette: 66 hex digits, eleven `rrggbb` colours run together in this order: background, cards, text, secondary text, accent, green, amber, red, bar track, Clawd, face eyes (`firmware/src/theme_palette.h`). The board repaints live and keeps it in NVS; a string of any other length or with a non-hex digit is ignored. The Clawd colour paints the screensaver face and the sprite: at draw time the official art's body orange and the shaded side of his body become that colour and the same shade of it, while his eyes, props and scenery keep their own colours (`theme_clawd_art`; the frames are never changed). The app sends a preset theme's palette (for the System theme, Dark or Light by the computer's appearance) or the user's own colours; the board doesn't know which. Boards with a 1-bit panel (`BoardCaps.mono_panel`, the e-paper board) ignore it and keep the default palette. Absent = keep the stored palette. |
