---
name: capturing-desktop-screenshots
description: Captures screenshots of the Clawdmeter desktop app (GPUI window) without hardware, using the demo example with fake board and status data under X11, and regenerates the reference set in desktop/docs/screenshots/. Use when checking a desktop UI change visually, when a desktop PR needs screenshots, or when asked to refresh the app's screenshots.
---

# Capturing desktop screenshots

The demo example (`desktop/crates/clawdmeter-app/examples/demo.rs`) opens the
real window with a fake board, a fake live status and a local release. The
bundled script runs it from a throwaway `HOME`, captures the window and
closes it:

```bash
.claude/skills/capturing-desktop-screenshots/scripts/capture.sh /tmp/device.png --page device
```

Then Read the PNG. It builds the demo first if
`desktop/target/debug/examples/demo` is missing (set `DEMO_BIN` to use
another build). It needs an X server or XWayland on `DISPLAY` (default `:0`),
`xwininfo` and ImageMagick's `import`. The window opens on the user's desktop
for a few seconds.

Why the script does what it does: the demo writes a status file, config and
release folder into `HOME`; the binary is copied into that `HOME` so hook
commands in the Hooks preview show no personal paths; it waits 5 s (`WAIT`)
because on X11 the dark appearance arrives from the portal after the first
frames.

## Demo flags

| Flag | Shows |
| --- | --- |
| `--page overview\|device\|flash\|accounts\|hooks\|settings` | A sidebar page |
| `--setup welcome\|board\|flash\|pair\|hooks\|done` | A setup-wizard step |
| `--board none\|fresh\|current\|old` | No board / board needing firmware / up to date / outdated |
| `--flash-to identify\|firmware\|flash\|done` | The flasher advanced to a step |
| `--status off` | The daemon not running |
| `--accounts retrying` | A second, rate-limited account |
| `--refresh wait\|checking` | Overview refresh button waiting out the gap / mid-check |
| `--session idle` | Session not started |
| `--hooks-preview`, `--shop`, `--advanced` | Hooks file preview, board shop links, advanced settings |
| `--height PX` | A taller window (default 640) so a long page fits one capture |

`demo.rs`'s header comment is the source of truth if these drift.

## Reference set

`desktop/docs/screenshots/` is made with this script. File names map to the
flags (`setup-2-board-shop.png` = `--setup board --shop`,
`overview-refresh-wait.png` = `--refresh wait`, …). `device.png` is taken
with `--height 1160` so the whole settings list shows.
`overview-gnome-header.png` is the client-drawn Adwaita header bar: run with
`CLAWDMETER_DECORATIONS=client` in the environment.

Don't add `observe_window_appearance` while working on the UI: on X11, GPUI
0.2.2 redraws inside the portal callback and panics. The root polls the
appearance instead.
