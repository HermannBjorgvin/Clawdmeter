# BLE protocol

Usage readings and hook events reach the app independently. The app selects
an account, builds a JSON payload, and writes it to the device over Bluetooth
LE. The firmware updates the meters and passes activity to Clawd's director.
Keyboard button reports use the separate HID service.

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
| `s`, `w` | Session and weekly % used. |
| `sr`, `wr` | Minutes until each resets; `-1` = window not started / unknown. |
| `st` | `allowed`, `limited` or `overage` (stored, not shown). |
| `acct` | `pro`, or `ent` for a spend-capped account: `s` is then the spend %, and `tp` (% of the billing period elapsed), `pd` (period length in days) and `rd` (reset date, e.g. `"Nov 1"`) fill the second card. |
| `t`, `tf` | Local wall-clock epoch and 12/24-hour format, when the clock is on. |
| `c` | `1` = the reset chime may sound. |
| `ok` | `false` alone = no usable account data; do not treat this as a zero reading. |
| `b`, `m`, `x` | Attention: alert chime, message (ASCII), end the alert. The app sends a fixed line per alert kind in `m`: `Question for you`, `Permission needed!` (also for other notification types), `Waiting for you!` (idle) or `MCP request!`; the test alert sends `Clawdmeter test alert`. The firmware shows whatever `m` it gets. |
| `a`, `e`, `es`, `n` | Fast clock: activity 0–3, latest hook event (`p` prompt, `s` stop, `b` back after a gap) and its sequence number, active sessions. |
| `rp` | `1` = a heartbeat replay, not a fresh poll; the rate math skips it. |
