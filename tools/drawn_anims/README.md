# drawn_anims — hand-drawn animations, dropped in as JSON

Export one `.json` per animation into this directory and run
`node tools/convert_to_c.js`. No index file to maintain, no conversion step:
every `.json` here that doesn't start with `_` is picked up by globbing.

This directory exists so that an external editor has somewhere safe to write.
The other two sources are owned by generators that **wipe and rewrite them**:
`claudepix_data/` belongs to `scrape_claudepix.js`, `custom_anims/` belongs to
`make_custom_anims.js`. A hand-drawn file in either would be deleted the next
time its generator ran. Nothing ever deletes this directory.

## Format

```json
{
  "name": "stretch",
  "category": "Idle",
  "description": "He gets up and has a stretch.",
  "palette": ["transparent", "#D97757", "#0F0F0F", "#FFC2D1"],
  "frames": [
    { "hold": 300, "grid": [[0,0,0, ...20 ints...], ...20 rows...] },
    { "hold": 120, "grid": [[0,0,0, ...], ...] }
  ]
}
```

| field | rule |
|---|---|
| `name` | unique; the string `splash.cpp` matches on |
| `category` | `Idle` \| `Work` \| `Dance` \| `Expressions` — label only, doesn't affect playback |
| `palette` | max **10** entries; `[0]` is the empty cell and should be `"transparent"` |
| `grid` | exactly **20 × 20** integers, each an index into `palette` |
| `hold` | milliseconds this frame stays on screen; per-frame, not a global frame rate |

`convert_to_c.js` validates all of this and fails loudly. The firmware reads
these arrays with fixed strides and no bounds checks, so a 19-row grid isn't a
wrong picture — it's a device reading past the end of an array.

## Two things that aren't validated but matter

**Colours must be light.** The panel background is pure black. `#0F0F0F` is
what the existing art uses for *eyes* — it reads as a hole in the body, not as
a dark colour. Anything dark is effectively invisible.

**Being in this directory isn't enough to appear.** An animation is only
reachable if its `name` is listed in `GROUP_NAMES` in `firmware/src/splash.cpp`.
Miss that and it compiles in, occupies flash, and is never once shown.

## Design notes for whoever draws

Per-frame holds are what make these feel alive — the existing set ranges from
80 ms to 700 ms in a single animation, with long holds on rest poses and short
ones on blinks. A uniform frame rate loses that.

Frame count matters less than you'd think: `dance bob` and `work type` are four
frames each and read fine on the panel. Four to six frames is a complete
animation; start there.
