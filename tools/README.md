# Asset tools

Run the commands from the repository root. They need Node.js; the splash
converter also needs ImageMagick, and the icon converter `pngjs`
(`npm install --no-save pngjs` in `tools/`, which is git-ignored).

## Splash animations

```bash
node tools/convert_official_clawd.js
node tools/convert_official_clawd.js --verify /tmp/verify   # + per-animation PNGs
```

Converts the official Anthropic Clawd animations archived in
`research/clawd-official/` (GIFs decoded via ImageMagick, the Laptop and
Soccer Lottie exports read directly) into a single
`firmware/src/splash_animations.h`:

- frames as bounding-box crops on the shared 55×37 art stage, one byte per
  cell into a per-animation ≤16-color RGB565 palette (index 0 = background)
- per-frame hold in ms, with consecutive duplicate frames collapsed
- a detected loop region per animation (gait cycles, scene middles) that the
  engine can hold or release for walk-to-target and timed scenes
- the eyes — transparent holes in the source GIFs — inked as `#141413`
- contrast recolors (trumpet notes → ivory, magnifier fedora → gray) and the
  sailing-loop cross-match that defines the sailing scene's loop window

See `research/clawd-official/CLAUDE.md` for asset provenance and the format
details, and `--in` / `--out` to override paths. Rebuild firmware after
running.

## Icons

```bash
node tools/png_to_lvgl.js input.png symbol_name [W_MACRO] [H_MACRO] [--tint=RRGGBB | --no-tint]
```

Converts an alpha PNG to an LVGL RGB565A8 C array. Default tint is white —
Lucide PNGs ship black-on-transparent and would render invisible without it.
Paste the output into `firmware/src/icons.h`.

## Simulator grid

```bash
python3 tools/simgrid/simgrid.py serve        # web GUI + action API
python3 tools/simgrid/simgrid.py run SCRIPT   # headless, deterministic
```

Every supported panel's simulator on one clock, driven by one set of
actions. Needs SDL2 dev files and Pillow (ffmpeg for `.mp4`). See
[docs/sim-grid.md](../docs/sim-grid.md); tests:
`python3 -m unittest discover -s tools/simgrid`.
