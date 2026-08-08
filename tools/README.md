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

`tools/custom_anims/` is a third source dir on this branch, holding animations
composed by `make_custom_anims.js` by posing existing claudepix frames rather
than drawing new ones.

Every animation is embedded as a loadable sample, so an existing one can
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

### On the onion skin

It draws the previous frame underneath the current one, so it only shows where
the current frame is empty. Anywhere the two overlap, the current frame hides
it — which means a prop held against the body (a mug, a pair of headphones)
has its previous position covered by exactly the thing you're positioning it
against.

Dots, an outline and a blurred haze were all tried on top and all rejected for
the same reason: the frame is 20 cells wide, there is no spare visual room, and
anything added over the drawing has to be looked past to read the drawing.
Being fainter doesn't fix it.

The tools that solve this solve it with **layers**, not with a better overlay —
put the prop on its own layer and its previous position isn't covered by the
body, because the body isn't on that layer. Aseprite and Krita also tint past
and future frames different colours so a ghost never reads as current art. The
other half of the answer is older than any of them: animators judge motion by
**flipping** between two frames rather than by studying a static ghost, which
is what the arrow keys do here when nothing is selected.

This editor has no layers. It writes one 20x20 grid of palette indices per
frame because that is what the firmware reads, and adding layers means an
editor-side model that flattens on export — worth doing if animating props
becomes common, not worth faking with an overlay.

## 3. Convert to C

```bash
node convert_to_c.js
```

Reads `tools/claudepix_data/*.json`, `tools/custom_anims/*.json` and
`tools/drawn_anims/*.json` and emits a single
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

Two parties hold rights in what's in here, and this project holds neither.

The animations are adapted from [claudepix](https://claudepix.vercel.app) by
[@amaanbuilds](https://x.com/amaanbuilds), a site that states no license. The
character Clawd belongs to Anthropic. Several animations in
`tools/drawn_anims/` are hand-edited versions of the claudepix originals, so
they are derivative of both.

This is a non-commercial community project. It is **licensed by neither party
and licenses nothing to anyone** — publishing a derivative work under an open
license would be asserting a right this project doesn't have, so it doesn't.
Confirm your own use is appropriate before redistributing, and don't ship any
of it in a paid product.
