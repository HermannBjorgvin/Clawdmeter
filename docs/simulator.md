# Firmware simulator

The simulator runs the firmware loop in an SDL2 window, with simulated
Bluetooth input and audio. It exercises the meters, Clawd's director,
touch input, idle behavior, payload parsing, and usage-rate logic without
a board. The default window is 480×480.

## Contents

- Build and run
- Controls
- Scenarios
- Director walkthrough
- Scripted input and screenshots
- Every panel at once
- Other display sizes

## Build and run

Install SDL2 development files (`sudo apt install libsdl2-dev` on Debian
or Ubuntu; `brew install sdl2` on macOS), then run:

```bash
pio run -d firmware -e sim
cd firmware
.pio/build/sim/program
```

Run from `firmware/`: scenario paths are relative to that directory.
The simulator HAL and Arduino shims live in `firmware/src/boards/sim/`.

## Controls

| Input | Action |
| --- | --- |
| Mouse click or left-drag | Touch the screen. On the meters, bring up Clawd's face; tap again to return. During a show or the screensaver, send him back to his seat. A waking tap only wakes the panel. |
| Space | Pause or resume scenario playback. |
| Left / Right | Step to a scenario state and pause playback. |
| 1–9 | Jump to the corresponding state, if present, and pause. |
| D | Toggle the simulated Bluetooth connection. |
| B / N, held | Primary / secondary hardware button. |
| P | PWR button. Hold about 3 seconds and release for the pairing gesture. |
| C | Toggle charging. |
| U | Toggle USB-only power with no battery. |
| − / = | Lower / raise battery level by 5 percentage points. |
| E / T | Inject a prompt / stop event. |
| A | Cycle activity from 0 through 3. |
| M | Toggle an attention alert. |
| S | Save a BMP screenshot in the current directory. |
| Escape or window close | Quit. |

## Scenarios

`firmware/sim/scenario.jsonl` is the default. Each line is a device payload,
with optional `name` (window title) and `hold_ms` (default 3000). Lines
beginning with `#` are comments. Playback loops. A missing file falls back
to built-in states.

```json
{"name":"steady use","hold_ms":10000,"s":42,"sr":180,"w":33,"wr":7900,"ok":true,"a":2,"e":"p","es":2,"n":1}
```

Use `SIM_SCENARIO=sim/my-scenario.jsonl` to choose a file. The payload goes
through the same parser and signal handling as hardware. `a` is activity
0–3; `e` is `p` (prompt), `s` (stop), or `b` (return after a gap); `es` is
the event sequence; `n` is the active-session count. The first `es` sets a
baseline. Each later change fires an event, including a change when the
scenario loops. `rp:1` marks a replay and excludes it from usage-rate math.
See the [payload reference](protocol.md) for usage fields.

## Director walkthrough

From `firmware/`:

```bash
SIM_FAST=1 SIM_SCENARIO=sim/director.jsonl .pio/build/sim/program
```

`SIM_FAST=1` compresses the director's minute-scale timers into seconds.
The walkthrough covers a prompt reaction, show, long-turn wave,
screensaver, wake, attention alert, and reset celebration. Typical event
times are about 14, 53, 90, 97, and 115 seconds for show, screensaver,
wake, attention, and reset. Scene selection can shift later steps; use
`SIM_TRACE=1` for the actual timestamps.

The interruption scenario covers a shortened lurk trip, an alert during
the screensaver, and a button press during a calm dip:

```bash
SIM_FAST=1 SIM_SCENARIO=sim/director-resume.jsonl \
  SIM_CMDS="2500:clawd lurk;5200:prompt;34000:alert;67500:pwr;80000:prompt" \
  .pio/build/sim/program
```

Character rules and expected transitions are in
[clawd-director.md](clawd-director.md).

## Scripted input and screenshots

For one headless capture:

```bash
SDL_VIDEODRIVER=dummy SIM_AUTOSHOT_MS=6000 .pio/build/sim/program
```

The simulator saves `sim-autoshot.bmp` and exits. Set `SIM_AUTOSHOT_PATH`
to choose another destination. For multiple captures and a touch event:

```bash
SDL_VIDEODRIVER=dummy SIM_FAST=1 SIM_SCENARIO=sim/director.jsonl \
  SIM_SHOTS="19500:blink.bmp,26000:scene.bmp,57000:face.bmp" \
  SIM_TAPS="25000@300,250" .pio/build/sim/program
```

| Variable | Format and behavior |
| --- | --- |
| `SIM_SHOTS` | `ms:path,ms:path`; ascending times, exit after the last shot. |
| `SIM_TAPS` | `ms@x,y;ms@x,y`; a 120 ms press at each time. |
| `SIM_CMDS` | `ms:command;ms:command`; serial commands, including `clawd peek <x>`, `liveliness <1-5>`, `attention_anim <seconds>` and `pace_marker <0|1>`. `pwr`, `prompt`, `stop`, `alert`, and `link` inject their corresponding input. |
| `SIM_MINIMAL=1` | Set the simulator's `BoardCaps.minimal_ui` at startup to preview a minimal-display board. |
| `SIM_BUTTONS=1` | Play a board with BOOT only (`BoardCaps.button_count = 1`); `N` does nothing. |
| `SIM_CIRCLE=1` | Set `BoardCaps.round_panel` to preview a round panel; the corners outside the circle are drawn as a grey bezel. |
| `SIM_TRACE=1` | Log director transitions and steps. |
| `SIM_NO_TOUCH=1` | Play a board without a touchscreen: `B` (BOOT) dismisses an active alert before it does anything else. |
| `SIM_NO_PWR=1` | Play a board without a PWR button (like the LCD-4): `P` does nothing and the pairing hint offers the touch-hold gesture. |
| `SIM_NO_BATTERY=1` | Start on USB power with no battery fitted (same as pressing `U`). |
| `SIM_EDGE_WATCH=1` | Log non-black pixel counts in the bottom 16 rows and right 16 columns to expose stray edge rendering. |
| `SIM_ROUND_EVEN=1` | Apply CO5300-style even-aligned flush rounding. |

Use `SIM_CMDS="9000:perf"` to print redraw costs for the last face moves
and performance. Desktop timings do not predict panel frame rates;
pixels per frame are useful for comparing rendering work.

## Every panel at once

`tools/simgrid/` runs one simulator per supported panel on a shared virtual
clock, with a web GUI, a command line and a headless script runner: see
[sim-grid.md](sim-grid.md). It drives each sim through controller
mode (`SIM_CTRL_FD`, `firmware/src/boards/sim/sim_ctrl.cpp`), where time
moves only on the host's `advance` and inputs come from the host.

## Other display sizes

`SIM_LCD_W` and `SIM_LCD_H` are compile-time defines, not runtime variables.
Use a separate build directory for each size:

```bash
PLATFORMIO_BUILD_DIR=/tmp/clawdmeter-sim-240 \
PLATFORMIO_BUILD_FLAGS="-DSIM_LCD_W=240 -DSIM_LCD_H=240" \
pio run -d firmware -e sim
cd firmware
/tmp/clawdmeter-sim-240/sim/program
```

For a round panel, build the square size and run with `SIM_CIRCLE=1`
(for example 466×466 for the AMOLED-1.32 and 1.43).

The simulator checks shared layout and behavior. It cannot validate a
physical panel's column offsets, bus timing, touch mapping, or rotation.
Check those on the target board before merging a port or display-driver change.
