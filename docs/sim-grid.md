# Simulator grid

`tools/simgrid/` runs the firmware simulator for every supported panel at
once: the boards on main and the open port PRs' panels, round ones masked
to the circle. They share one clock, one payload stream and one set of
commands, so an alert, a Clawd beat or a usage change shows on every board
at the same moment. A web GUI and a command line drive the same actions.

It needs SDL2 dev files (as the simulator does), Python 3 with Pillow
(`python3-pil` / `pip install pillow`), and `ffmpeg` only for `.mp4`
recordings.

## How it works

Each cell is a simulator process built at its panel size
(`firmware/.pio/simgrid/<WxH>/`) and started in controller mode
(`SIM_CTRL_FD`, `firmware/src/boards/sim/sim_ctrl.cpp`). In that mode its
time is virtual: it starts at 0, moves only when the host says `advance`,
and touch, buttons, the BLE link and payloads come from the host. The host
(`tools/simgrid/engine.py`) advances every process in lockstep, plays the
desktop app's part (full payloads, a 60 s heartbeat, the clock), and turns
the panels' framebuffers into PNGs, GIFs and MP4s. The same script of
actions gives the same pixels on every run.

`tools/simgrid/profiles.py` lists the cells: one per distinct panel size and
set of capabilities (round, PWR button, button count, minimal mode),
labelled with the boards each stands for. Add a board there when a port
brings a new size or capability.

## Iterating on a UI change (agents)

1. Make the change, then run a script headless. Sim builds that are out of
   date rebuild first (the first build of all sizes takes a couple of
   minutes; after that a script runs in about a second):

   ```bash
   python3 tools/simgrid/simgrid.py run tools/simgrid/examples/layouts.txt
   ```

2. Read the PNGs it printed. Every board is in each one, labelled.

3. For behaviour over time, record it and look at frames:

   ```bash
   python3 tools/simgrid/simgrid.py run tools/simgrid/examples/attention.txt
   ```

4. To compare before and after, run the same script on both versions: the
   pictures are identical unless the change shows.

Write scripts to your scratch directory. One action per line, the same
words as the command line:

```text
preset enterprise          # usage presets: usage fresh high maxed enterprise not-started nodata
payload s=88 w=40          # any payload fields; b/m/x/c act once
step 5000                  # advance every board's clock by 5 s
beat attention             # clawd show|screensaver|attention|joy|lurk|home|peek
alert "Bash: rm -rf build"
tap round 233 300 hold_ms=5000   # one board, in its panel's pixels
press lcd4 boot hold_ms=150      # one board's button: boot, secondary, pwr
link false                 # BLE down on every board (the pairing hint)
liveliness 5
shot /path/grid.png scale=0.5    # board=<id> for one panel
record /path/clip.gif fps=15 scale=0.4
step 8000
record_stop
```

`--boards 154,round` (ids or sizes, or `main`) limits a run to some cells.
`simgrid.py actions` lists every action and its arguments; `simgrid.py
boards` the cells.

## Interactive

```bash
python3 tools/simgrid/simgrid.py serve            # opens http://127.0.0.1:8765/
```

The sidebar has every action: play/pause and step the clock, usage presets
and sliders, stale data, the 12-hour clock, the pace marker, activity, hook events, alerts,
Clawd beats, liveliness, the BLE link, battery, serial commands, scenarios
(`firmware/sim/*.jsonl`), screenshots and recordings of the grid or one
board. Click a panel to touch it (hold for a long touch); right-click it for
its buttons.

While `serve` runs, the command line talks to it, so a human and an agent
can share one grid:

```bash
python3 tools/simgrid/simgrid.py beat attention
python3 tools/simgrid/simgrid.py shot /tmp/now.png
python3 tools/simgrid/simgrid.py pause
python3 tools/simgrid/simgrid.py step 250
python3 tools/simgrid/simgrid.py status
```

`serve --paused` starts with the clock stopped, so captures stay
reproducible. The HTTP API is `POST /api/<action>` with
`{"args": [...], "kwargs": {...}}`, `GET /api/status`, `GET /frame/<id>.png`
and `GET /grid.png`.

## Limits

The grid is the simulator: desktop LVGL, fake data, no panel quirks
(column offsets, rotation, flush rounding) and no audio. It doesn't replace
a check on hardware. The C6 boards share their S3 sibling's cell; the sim
always behaves like a PSRAM board.
