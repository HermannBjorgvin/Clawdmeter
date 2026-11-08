---
name: flashing-and-probing-boards
description: Flashes firmware to a connected Clawdmeter board over USB and talks to it over serial - upload commands, picking the port, and the serial command set (info, ble, unpair, disconnect, perf, chime tuning, clawd beats). Use when asked to flash or upload firmware, read the board's state, debug pairing or BLE on a device, tune the chime, or run any serial command on hardware.
---

# Flashing and probing boards

Only flash when a board is attached and the user asked for it: an upload
replaces whatever the board runs. Serial commands are safe to use for QA
without asking, except `unpair` and `disconnect`, which drop the user's
pairing or link.

## Flash

```bash
pio run -d firmware -e <env> -t upload --upload-port /dev/ttyACM0
```

Ports: `/dev/ttyACM*` on Linux, `/dev/cu.usbmodem*` on macOS, `COMn` on
Windows. "This chip is ESP32-C6, not ESP32-S3" (or the reverse) means the env
doesn't match the board; envs and boards are listed in the root `CLAUDE.md`.
If `pio` isn't on PATH: `~/.platformio/penv/bin/pio`. Release images are
merged binaries flashed at `0x0`; the desktop app's Flash page writes those.

## Serial commands

Send one and print the reply:

```bash
python3 ${CLAUDE_SKILL_DIR}/scripts/serial_cmd.py info
python3 ${CLAUDE_SKILL_DIR}/scripts/serial_cmd.py clawd show --port /dev/ttyACM1
```

The script picks the only attached board, waits until the board goes quiet,
and fails with a clear message when no board or several are attached. If
`pyserial` is missing, run it with `~/.platformio/penv/bin/python`. Close any
`pio device monitor` first; it holds the port.

| Command | Reply / effect |
| --- | --- |
| `info` | JSON: env, version, panel revision, link state |
| `ble` | Link state, bonds, owner, peers |
| `perf` | Redraw cost of the last face rise / settle / drop / performance |
| `clawd <show\|screensaver\|attention\|joy\|lurk\|home>` | Force a director beat |
| `clawd peek <x>` | The peek beat, as a tap at x would |
| `liveliness [1-5]` | Print or set Clawd's liveliness (kept in NVS) until the app sends its own `lv` |
| `attention_anim [seconds]` | Print or set how long Clawd points and waves for an alert (0 = off; kept in NVS) until the app sends its own `aa` |
| `pace_marker [0\|1]` | Print or set the pace marker on the usage bars (kept in NVS) until the app sends its own `pm` |
| `chime` (`buzz` is an alias) | Play the ding |
| `chime <gain> [volume]` | Tune the speaker until reboot: gain 0..1, ES8311 volume 0..100. The 2.16 ships 0.7 / 77 (tuned by ear); the 1.8, LCD-1.54 and C6 2.16 ship 0.7 / 75 (not tuned by ear); the M5StickS3 ships 0.7 / 65 |
| `unpair` | Clear bonds and re-advertise; replies `{"unpair":true}` |
| `disconnect` | Drop every BLE link |
| `screenshot` | Binary framebuffer dump; use the `capturing-device-screenshots` skill for a PNG |

The authoritative list is the command parser in `firmware/src/main.cpp`
(search `cmd_buf`); director beats are in `director_debug()` in
`firmware/src/clawd_director.cpp`.

For UI work, force a beat here, then capture it with the
`capturing-device-screenshots` skill.
