# Official Clawd animations — sourcing, classification, pivot plan

Context for future sessions. Goal: **ship Anthropic's official Clawd
animations as the device's splash/mascot art** (they replaced an earlier
third-party sprite set). `README.md` in this folder is the asset inventory;
this file records where the assets came from, how to get more, and the
classification that the firmware state machine is built on. The conversion
pipeline (`tools/convert_official_clawd.js`) and the 60×60-stage splash engine
implementing this are in the tree; see the root CLAUDE.md.

## Where the assets came from (methodology)

Four independent sources, cross-confirmed:

1. **claude.ai/code web app** (a user-captured Firefox HAR). The JS bundles
   reference `/images/clawd/…` paths, and the Clawd-Laptop Lottie was fetched
   at runtime from `https://assets-proxy.anthropic.com/claude-ai/v2/assets/v1/<hash>.json`.
2. **The Linux desktop .deb** — download without installing:
   the apt pool URL comes from
   `curl -s https://downloads.claude.ai/claude-desktop/apt/stable/dists/stable/main/binary-amd64/Packages | grep ^Filename`,
   then `dpkg-deb -x`. Inside, `usr/lib/claude-desktop/resources/ion-dist/` is
   a **local mirror of the claude.ai web bundles** — fully greppable, no
   Cloudflare. `grep -rhoE '/images/[A-Za-z0-9/_.-]+\.(gif|png|webp)' ion-dist/`
   yields the referenced asset list; the Laptop Lottie JSON is embedded in a
   bundle verbatim (search `"nm":"Clawd-`). Also ships `app.asar` (extract with
   `npx @electron/asar extract`), which holds the Electron shell but no Clawd art.
3. **Direct download from claude.ai.** Static assets are NOT Cloudflare-gated —
   `curl -A "Mozilla/5.0 …" https://claude.ai/images/clawd/core/Clawd-Waving.gif`
   just works. Only HTML routes (the SPA, `/animations/*.html`) get the
   challenge page.
4. **URL name probing.** The image server returns a real GIF for a valid name
   and an HTML catch-all (HTTP 200 either way) for anything else, so probing is
   trivial: request `…/Clawd-<Name>.gif` and check the magic bytes (`GIF8`).
   This surfaced 7 animations referenced by **no** shipped bundle (Soccer,
   Basketball, Skateboard, Trumpet, Walking, Jumping, Pointing) — presumably
   staged for seasonal promos (Soccer = FIFA World Cup 2026). `fetch.sh` here
   re-downloads everything; extend its probe list to hunt for new drops.

GitHub archaeology that helped: `companion-inc/pet-clawd` (an earlier archive
of the same sets, with in-app usage notes), `Mapleeeeeeeeeee/digest-showcase`
(preserved the Clawd-Soccer Lottie, an official Anthropic export no longer
referenced by the site), and `anthropics/claude-desktop-buddy` (official BLE
maker API — separate research thread, see the project memory).

## Format facts (verified, not assumed)

- **One logical stage for everything: 55×37 art-pixels.** Core GIFs are
  2750×1850 at 50 px/cell; persona GIFs are 1189×800 at **21.6 px/cell**
  (non-integer!). Extract cells by sampling centers
  (`x = round((i + 0.5) * W / 55)`), never by block averaging.
- Lotties (Laptop, Soccer) are pure axis-aligned 50 px rects, 12 fps, with
  per-group opacity keyframes selecting frames; both occupy 34×23 cells of the
  stage. Decoder lives in the artifact build script (scratchpad
  `build_page.js`) and is trivial to re-derive: paint groups bottom-to-top
  (last Lottie layer first) whose opacity at frame t exceeds 50.
- GIF timing: 80 ms/frame (older exports) or 90 ms (newer); Lotties 12 fps.
  Treat 12.5 fps as canonical.
- Palette: body `#D87656`, shade `#BE684D`, gray `#8B8B8B`, ivory `#F9F8F4`,
  eyes `#141413` (near-black), plus per-scene prop colors.
- **Quality tiers**: core set + Cloud/Lurking/boats are crisp (≤15 colors);
  Magnifier 13, RacingCar up to 22 (helmet/kart shading); the unlisted sports
  trio (Soccer/Basketball/Skateboard GIFs) are **antialiased non-integer
  rescales** (1188×798, hundreds of colors) — requantize against the palette,
  or better, use the crisp Clawd-Soccer Lottie as the source for Soccer.

## Classification for the state machine

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
  - **Empty-stage**: Lurking — frames 0 and 66 are a fully empty canvas. He
    peeks in from the edge and leaves. Precondition: walk Clawd off-screen
    first (CrabWalking/Walking + renderer translation), play Lurking, then
    walk him back in.
- **Scene middles are extractable loops.** Proof: sailing-loop frame 0 is
  pixel-identical (RMSE 0) to sailing-scene frame 26 — Anthropic themselves
  ship the extracted middle. To extend any scene, find its loop window the
  same way (self-similarity search across frames) instead of eyeballing.
- **Cloud-still is redundant** — byte-identical to the hold frames (20, 30,
  39…) of Cloud-once. Drop it; it's just the pose Cloud-once parks on.
- **Walks are in-place gaits** at a fixed canvas position (the content bbox
  alternates by exactly one cell). Screen travel = renderer translation, so
  walking speed is tunable independently of the gait: pick px/frame per board
  size and per gait (side-scuttle vs front walk read differently) on hardware.

## ESP32 feasibility (analysis only — needs hardware validation)

- **Do not decode GIF on-device.** A coalesced persona frame is 1189×800 —
  full-frame decode buffers don't fit, and C6 boards have no PSRAM. Instead
  convert offline (`tools/convert_official_clawd.js`):
  GIF/Lottie → 55×37 palette-indexed cell frames at build time.
- Budget: all 17 animations ≈ 830 frames. 55×37 = 2035 cells/frame ≈ 1 KB at
  4 bpp (16-color palette) → **< 1 MB flash for the entire catalog**, before
  cropping to per-animation bounding boxes or run-length encoding. Easily fits
  every board (4–32 MB flash).
- Current splash engine is fixed 20×20 (`CELL = min(W,H)/20`); official art
  needs 55×37 (or per-animation cropped grids) plus a translation offset for
  walk travel. Engine generalization is an **open design decision** —
  explicitly deferred, do not implement without discussing.
- 4-bit palette (16 colors) covers everything except RacingCar (22) — either
  requantize its rarest shades or allow one 5-bpp animation.
- On-device playback rate: 12.5 fps × ~2 KB blits is trivial next to the
  existing 480×480 LVGL pipeline; the risk is not throughput but RAM spikes —
  keep frames palette-indexed in flash, expand row-by-row during blit.

## Conversion decisions (implemented in tools/convert_official_clawd.js)

- `7bbe5052.gif` (sailing loop) is decoded but **not emitted** — it exists to
  locate the sailing scene's loop window by cross-match; standalone it would
  start mid-scene with no intro.
- Eyes are synthesized: the GIFs draw them as transparent holes (claude.ai
  pages show through). Interior background components get `#141413` ink.
- Contrast recolors on the dark device ground: trumpet's floating note
  components → ivory `#F9F8F4`; magnifier's fedora `#141413` → gray `#8B8B8B`
  (lens interior excluded — enclosed by the rim, it never touches background).

## Total footprint (as archived here)

GIFs 4.77 MB · PNGs 0.22 MB · Lottie JSONs 1.28 MB · webm/svg 0.03 MB —
**6.30 MB total** (source assets; the converted on-device format is ~1 MB).
