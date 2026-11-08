---
name: verifying-firmware-ui
description: Checks firmware UI and Clawd behaviour changes visually - the simulator grid (every panel at once, scripted, deterministic), single-sim screenshots (SIM_SHOTS, SIM_CMDS, SIM_TAPS), director walkthroughs, then `clawd <beat>` + `screenshot` on a connected board, with the C6 no-screenshot limit. Use after any change to the meters, Clawd, the face, layouts or LVGL drawing, when asked to screenshot the device or simulator, or before opening a firmware UI PR.
---

# Verifying firmware UI

Check your own UI changes; don't ask the user to look first. Read every
capture you take. The simulator is the first pass, real hardware the last.

## Simulator grid (start here)

`tools/simgrid/` runs the sim for every supported panel at once on one
virtual clock (guide: [docs/sim-grid.md](../../../docs/sim-grid.md)). Write a
script of actions to your scratch dir and run it headless; it rebuilds stale
sims, prints the files it saved, and gives the same pixels every run:

```bash
SCRATCH=/your/scratch/dir
cat > "$SCRATCH/check.txt" <<EOS
step 6000
shot $SCRATCH/meters.png
beat attention
record $SCRATCH/attention.gif fps=12 scale=0.4
step 8000
record_stop
link false
step 500
shot $SCRATCH/pairing.png
EOS
python3 tools/simgrid/simgrid.py run "$SCRATCH/check.txt"
```

Read every PNG, and
pull a few frames out of a GIF to check motion. `--boards 154,round` limits
the cells; `simgrid.py actions` lists everything a script can do (payload
fields, presets, alerts, events, beats, taps and button presses on one
board, the BLE link, battery, scenarios). For a before/after comparison,
run the same script on both versions.

## Single simulator

The sim runs the full firmware loop unmodified in an SDL2 window (director,
meters, idle fade, pair gesture, payload/signals, rate math, chime logic);
only `ble.cpp`/`chime.cpp` are stubs. It needs SDL2 dev files
(`libsdl2-dev` / `brew install sdl2`); if they're missing, ask the user to
install them. Build from the repo root, run from `firmware/`:

```bash
pio run -d firmware -e sim
cd firmware
SDL_VIDEODRIVER=dummy SIM_FAST=1 SIM_SCENARIO=sim/director.jsonl \
  SIM_SHOTS="19500:/tmp/shot-a.bmp,57000:/tmp/shot-b.bmp" .pio/build/sim/program
```

Write captures outside the repo (a scratch dir), convert BMP → PNG if needed
(`convert in.bmp out.png`), and Read them.

| Variable | Use |
| --- | --- |
| `SIM_FAST=1` | Director minutes → seconds (`clawd_tuning_fast()`) |
| `SIM_SCENARIO=sim/<file>.jsonl` | Payload script; `director.jsonl` hits every beat, `director-resume.jsonl` interruptions |
| `SIM_SHOTS="ms:path,…"` | Screenshots at ascending times; exits after the last |
| `SIM_CMDS="ms:cmd;…"` | Serial commands, e.g. `clawd show`, `clawd peek <x>`, `liveliness 1`; `pwr`, `prompt`, `stop`, `alert` press the button/keys |
| `SIM_TAPS="ms@x,y;…"` | Touches; `ms@x,y+hold_ms` for a touch hold |
| `SIM_TRACE=1` | Director transitions with timestamps (find when a beat actually happens) |
| `SIM_NO_PWR=1` / `SIM_NO_TOUCH=1` / `SIM_MINIMAL=1` | A board without a PWR button / without a touchscreen / `BoardCaps.minimal_ui` |
| `SIM_CIRCLE=1` | A round panel (`BoardCaps.round_panel`); the corners are drawn as a bezel |
| `SIM_AUTOSHOT_MS`, `SIM_AUTOSHOT_PATH` | One headless shot (what CI captures) |

Full key map, scenario format and the rest of the variables:
[docs/simulator.md](../../../docs/simulator.md).

**Other panel sizes** are compile-time defines, so each size needs its own
build dir (never the scratchpad root):

```bash
PLATFORMIO_BUILD_DIR=/tmp/clawdmeter-sim-240 \
PLATFORMIO_BUILD_FLAGS="-DSIM_LCD_W=240 -DSIM_LCD_H=240" pio run -d firmware -e sim
(cd firmware && /tmp/clawdmeter-sim-240/sim/program)
```

Check every size your change can affect: 480×480 and 410×502 (large
design), 368×448 (compact), 240×240 (small), and for layout changes the port
sizes in `firmware/test/test_layout` (466×466 with `SIM_CIRCLE=1`, 450×600,
320×240, 240×320, 170×320, 135×240, 200×200, …).

**Layout changes** (`layout.cpp`, `ui_text.h`, anything that moves or resizes
meters text): run `pio test -d firmware -e native -f test_layout` first. It
checks every supported panel shape with the real fonts and names what leaves
the glass, overlaps or doesn't fit. Then diff before/after sim captures at
the four sizes on main: they should be pixel-identical unless the change is
meant to show there. Attention captures vary run to run (sprite and scroll
timing), so compare those against several base runs.

**Walkthroughs** (from `firmware/`):

```bash
SIM_FAST=1 SIM_SCENARIO=sim/director.jsonl .pio/build/sim/program
#   notice ~3 s, show ~14 s, screensaver ~53 s, wake ~90 s, attention ~97–110 s, reset joy ~115 s
SIM_FAST=1 SIM_SCENARIO=sim/director-resume.jsonl \
  SIM_CMDS="2500:clawd lurk;5200:prompt;34000:alert;67500:pwr;80000:prompt" .pio/build/sim/program
```

Scene choice shifts later timings; use `SIM_TRACE=1` rather than guessing.
Reference captures live in `screenshots/director/`; expected behaviour is in
[docs/clawd-director.md](../../../docs/clawd-director.md).

## On hardware

The sim renders with desktop LVGL and fake data. **Always do a final check on
real hardware before merging panel-related changes**: column offsets,
rotation and flush rounding live in the board code, not shared code.

With a board attached and flashed (`flashing-and-probing-boards` skill), on
each UI iteration:

1. Force the state: `clawd <show|screensaver|attention|joy|lurk|home>` or
   `clawd peek <x>` over serial.
2. Capture it with the `capturing-device-screenshots` skill.
3. Read the PNG.

There's no need to edit the boot screen in `main.cpp` to reach a state.

**C6 boards can't be captured** (no PSRAM for the snapshot). Verify C6
changes in the sim at 480×480 / 368×448 and say in the PR that the C6 panel
needs eyeballing on hardware.

## Reporting

In the PR, label simulator captures as simulator output; they don't count as
hardware testing. Name the exact boards tested on hardware, or write
"not tested on hardware".
