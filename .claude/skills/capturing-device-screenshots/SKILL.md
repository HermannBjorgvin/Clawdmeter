---
name: capturing-device-screenshots
description: Captures a connected Clawdmeter board's screen as a PNG over USB serial, with the bundled screenshot.py (the firmware's `screenshot` framebuffer dump, decoded). Use when asked to screenshot a board or the device, to check UI on real hardware, or to attach device output to a PR.
---

# Capturing device screenshots

```bash
python3 ${CLAUDE_SKILL_DIR}/scripts/screenshot.py /path/out.png
```

Then Read the PNG. It's sized to the panel (480×480 on the AMOLED-2.16) and
takes about 2 s. Write it outside the repo unless it belongs in the PR.

- **Port**: the only `/dev/ttyACM*` or `/dev/cu.usbmodem*` attached; pass
  `--port` when there are several. Close any `pio device monitor` first, it
  holds the port. Opening the port doesn't reset the board.
- **pyserial missing**: run it with `~/.platformio/penv/bin/python`.
- **C6 boards can't be captured**: no PSRAM for the snapshot, so the script
  stops with a message. Eyeball those on the panel, or check the same UI in
  the simulator (`verifying-firmware-ui` skill).
- **A specific state**: force it over serial first (`clawd show`, `clawd
  attention`, … via the `flashing-and-probing-boards` skill), then capture.

Device captures show what the firmware drew, not the panel's colours or
offsets; a photo is still the proof for those.
