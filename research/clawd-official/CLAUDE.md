# Official Clawd animations

This folder archives Anthropic's official Clawd animations, the only source
of Clawd sprite art on the device. `README.md` is the inventory (files,
frames, timing, where each is used). `fetch.sh` re-downloads everything and
probes for new names. To find new animations, use the
`hunting-clawd-animations` skill; to convert them, the
`regenerating-firmware-assets` skill (`tools/convert_official_clawd.js`).

## Format facts (verified, not assumed)

- **One logical stage for everything: 55×37 art-pixels.** Core GIFs are
  2750×1850 at 50 px/cell; persona GIFs are 1189×800 at **21.6 px/cell**
  (non-integer). Extract cells by sampling centers
  (`x = round((i + 0.5) * W / 55)`), never by block averaging.
- Lotties (Laptop, Soccer) are pure axis-aligned 50 px rects, 12 fps, with
  per-group opacity keyframes selecting frames; both occupy 34×23 cells of the
  stage. Decode by painting, bottom to top (last Lottie layer first), every
  group whose opacity at frame t exceeds 50.
- GIF timing: 80 ms/frame (older exports) or 90 ms (newer); Lotties 12 fps.
  Treat 12.5 fps as canonical.
- Palette: body `#D87656`, shade `#BE684D`, gray `#8B8B8B`, ivory `#F9F8F4`,
  eyes `#141413` (near-black), plus per-scene prop colors.
- **Quality tiers**: core set + Cloud/Lurking/boats are crisp (≤15 colors);
  Magnifier 13, RacingCar up to 22 (helmet/kart shading); the unlisted sports
  trio (Soccer/Basketball/Skateboard GIFs) are **antialiased non-integer
  rescales** (1188×798, hundreds of colors): requantize against the palette,
  or better, use a crisp Lottie source (Soccer has one).

## Classification

Verified frame-level findings (RMSE comparisons, bbox trims):

- **Idle is the universal hinge.** Every persona scene starts AND ends on
  plain standing Clawd; the bookend poses match across scenes to within
  3–5% RMSE (blink/offset variation only). Any scene can chain into any other
  through idle. Categories:
  - **Loops** (self-looping, no bookends): CrabWalking, Walking, Dancing,
    Jumping, JumpingHappy, Pointing, Waving, sailing-loop (`7bbe5052`).
  - **Scenes** (idle → prop assembles → loopable middle → prop packs away →
    idle): RacingCar, both sailing files, Cloud-once, Magnifier, Trumpet,
    Basketball, Skateboard, Soccer.
  - **Empty-stage**: Lurking. Frames 0 and 66 are an empty canvas: he peeks
    in from the edge and leaves. Walk Clawd off-screen first, play Lurking,
    then walk him back in.
- **Scene middles are extractable loops.** Sailing-loop frame 0 is
  pixel-identical (RMSE 0) to sailing-scene frame 26: Anthropic ship the
  extracted middle themselves. Find any scene's loop window by
  self-similarity search across frames, not by eye.
- **Cloud-still is redundant**: byte-identical to the hold frames (20, 30,
  39…) of Cloud-once, the pose Cloud-once parks on.
- **Walks are in-place gaits** at a fixed canvas position (the content bbox
  alternates by exactly one cell). Screen travel is renderer translation, so
  walking speed is tuned per board size and per gait, independent of the art.

## Conversion decisions (implemented in the converter)

- Never decode GIFs on the device: a coalesced persona frame is 1189×800 and
  C6 boards have no PSRAM. Frames are converted offline to palette-indexed
  cells and expanded row by row during the blit.
- `7bbe5052.gif` (sailing loop) is decoded but **not emitted**: it locates the
  sailing scene's loop window by cross-match; standalone it would start
  mid-scene with no intro.
- Eyes are synthesized: the GIFs draw them as transparent holes (claude.ai
  pages show through). Interior background components get `#141413` ink.
- Contrast recolors on the dark device ground: trumpet's floating note
  components → ivory `#F9F8F4`; magnifier's fedora `#141413` → gray `#8B8B8B`
  (lens interior excluded: enclosed by the rim, it never touches background).
- Each animation gets one ≤16-color palette (index 0 = background). Extra
  colors (RacingCar's 22, antialiased rescales) cluster into the dominant
  crisp colors or snap to the nearest entry.

Source assets total about 6.3 MB here; the converted frame data is about 400 KB.
