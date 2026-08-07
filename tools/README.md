# Splash animation tools

Pipeline for getting 20×20 pixel animations — scraped or your own — onto the device.

## 1. Scrape

```bash
node scrape_claudepix.js
```

Fetches the manifest from `claudepix.vercel.app/app.js`, then each animation's
HTML file, evaluates the embedded JS in a Node VM context (loading the same
`creature-engine.js` the site uses), and writes resolved frame data to
`tools/claudepix_data/*.json`.

Each output file looks like:
```json
{
  "filename": "idle_breathe.html",
  "name": "idle breathe",
  "category": "Idle",
  "description": "...",
  "frame_count": 17,
  "frames": [{ "hold": 500, "grid": [[0,0,...],[0,1,1,...],...] }, ...]
}
```

Override URL or output dir with `--base` and `--out`.

## 2. Draw or fix animations (optional)

`anim_editor.html` is a 20×20 animation editor. Open it straight off disk — one
file, no dependencies, no server, nothing fetched.

It carries what the firmware cares about rather than what a general pixel editor
offers: 20×20 and the 10-colour cap enforced, per-frame hold times, onion skin,
playback at the real holds, and both device previews on black (24px/cell splash,
4px/cell corner badge). A colour that looks fine on white can vanish on the
panel and a shape that reads at 24px can turn to mush at 4px, so previewing at
both scales is the point.

Every scraped animation is embedded as a loadable sample, so an existing one can
be opened and fixed rather than rebuilt. Grids and holds round-trip exactly; the
palette does not. Hex is uppercased, and the claudepix body colour `#CD7F6A`
comes back as `#D97757` — the editor is shown the colour the device displays,
not the one in the source file, and `convert_to_c.js` applies the same remap on
the way to C, so nothing changes on the device. It only matters if you hand an
export back to claudepix as if it were their original file.

Export lands in `tools/drawn_anims/`, which the converter reads — see that
directory's README for how a file there replaces a scraped one.

Re-run `node build_editor_samples.js` after changing any animation. It rewrites
only the region between the `BEGIN-SAMPLES` / `END-SAMPLES` markers; the rest of
the editor is hand-written. The data is inlined rather than fetched because the
editor runs from `file://`, where fetching a sibling file is blocked.

The UI is in Chinese.

## 3. Convert to C

```bash
node convert_to_c.js
```

Reads `tools/claudepix_data/*.json` and `tools/drawn_anims/*.json` and emits a single
`firmware/src/splash_animations.h` with:
- `splash_<ident>_frames[N][400]` — per-frame cell codes (0 = empty, 1 = body, 2 = eye)
- `splash_<ident>_holds[N]` — per-frame hold time in ms
- `splash_anims[]` — master table with name, category, frame count, pointers
- `SPLASH_ANIM_COUNT`

The firmware (`splash.cpp`) consumes this header to render and animate.

## Re-running

The scraper is idempotent — re-run any time the source library updates. The
converter overwrites the header. Rebuild firmware after running both.

## License note

The scraper hits a public site without a stated license. Confirm reuse is
appropriate for your case before redistributing the output.
