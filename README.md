# Clawdmeter (macOS · 1.8" board)

A desk-side ESP32 dashboard that shows your Claude Code usage in real time, beeps when Claude is waiting on you, and lists every open CC session so you never lose track of one.

> Fork of [HermannBjorgvin/Clawdmeter](https://github.com/HermannBjorgvin/Clawdmeter) ported to the **Waveshare ESP32-S3-Touch-AMOLED-1.8** board and to **macOS** (upstream targets the 2.16" board + Linux/BlueZ).

|       Usage       |    Sessions    |    Settings    |    Splash    |
| :---------------: | :------------: | :------------: | :----------: |
| % of session +<br/>weekly used | All open<br/>CC sessions | Volume +<br/>BLE + mute | Pixel Clawd<br/>animations |

## How it works

1. ESP32 advertises a BLE GATT service.
2. A Python daemon on your Mac connects, pulls the Claude Code OAuth token from the **macOS Keychain**, hits `api.anthropic.com/v1/messages` with a 1-token request every 60s, and parses the `anthropic-ratelimit-unified-*` response headers.
3. Claude Code hooks (`Notification`, `Stop`, `UserPromptSubmit`, etc.) write per-session state files. The daemon picks up changes within 1s and pushes them to the device.
4. When any CC session enters waiting state, the device pops a full-screen attention overlay and plays a two-tone chime.

## Hardware

- [Waveshare ESP32-S3-Touch-AMOLED-1.8](https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.8) — ESP32-S3R8, 1.8" 368×448 AMOLED (SH8601 QSPI), FT3168 cap touch, AXP2101 PMU, QMI8658 IMU, ES8311 audio codec.
- USB-C cable for flashing + power.
- Optional 3.7V Li-Po (MX1.25 connector) for untethered use.

## Install on a fresh Mac

```bash
git clone https://github.com/alecfeeman/Clawdmeter.git
cd Clawdmeter

# 1) Flash the firmware (board plugged into Mac via USB-C)
brew install platformio
pio run -d firmware -t upload --upload-port "$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)"

# 2) Install the daemon + Claude Code hooks
./daemon/install-macos.sh
```

The installer:
- Creates a Python venv at `~/.clawdmeter-venv` with `bleak`.
- Patches `~/.claude/settings.json` to wire 7 hook events (idempotent — existing hooks are preserved).
- Optionally installs a LaunchAgent at `~/Library/LaunchAgents/com.clawdmeter.daemon.plist` so the daemon auto-starts at login.

On first run, macOS will prompt twice:
- **Bluetooth permission** for the daemon process — approve.
- **Keychain access** for `Claude Code-credentials` — click "Always Allow."

Tail the daemon log: `tail -f ~/Library/Logs/clawdmeter.log` (if you installed the LaunchAgent).

## UX

- **Swipe left / right** — cycle through Usage → Sessions → Settings.
- **Swipe up** from any screen — show the Clawd splash animation.
- **Swipe any direction** on splash — dismiss.
- **Tap** does nothing on the main screens (it conflicts with swipes). The red attention overlay can be tap-dismissed.
- **BOOT button** — toggle display sleep. Wake on any touch or incoming attention event.
- **PWR button** — short press: cycle screens / wake. Long press (~1.5s): power off via AXP2101.

## Multi-session support

Each CC session writes its own state file at `/tmp/clawdmeter-sessions/<session_id>.json`. The Sessions page shows up to 6 active sessions with a status dot (waiting / working / idle) and project name. If multiple sessions are waiting, the attention overlay shows the count plus the most recent message.

Stale session files (>24h old, from crashes or non-graceful exits) are auto-reaped.

## What's different from upstream

| | upstream (2.16", Linux) | this fork (1.8", macOS) |
|---|---|---|
| Display driver | CO5300 480×480 | SH8601 368×448 |
| Touch | CST9220 | FT3168 (via Arduino_DriveBus) |
| Display reset | direct GPIO | XCA9554 I/O expander |
| Daemon language | bash + bluetoothctl + busctl | Python + bleak |
| Credentials | `~/.claude/.credentials.json` | macOS Keychain (security CLI) |
| BLE bonding | required | disabled (avoids reflash mismatch) |
| Auto-start | systemd user unit | launchd LaunchAgent |
| Sessions screen | — | new |
| Attention overlay + chime | — | new |
| Audio | — | ES8311 + I2S beep |
| Volume/mute | — | new (Settings page) |
| Swipe navigation | — | new |

## Attribution

- Original project: [HermannBjorgvin/Clawdmeter](https://github.com/HermannBjorgvin/Clawdmeter)
- Pixel-art Clawd animations: [claudepix](https://claudepix.vercel.app) by [@amaanbuilds](https://x.com/amaanbuilds)
