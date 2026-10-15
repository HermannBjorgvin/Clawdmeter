# Official Anthropic Clawd mascot animations — research archive

Collected 2026-08-10 (two research rounds). These are the pixel-art Clawd
animations Anthropic ships in claude.ai/code (web) and the Claude Code desktop
app. All are served from `https://claude.ai/images/` (directly curl-able, no
auth — only HTML routes sit behind Cloudflare) except the Laptop Lottie, which
comes from the assets proxy.

`contact2.png` + `contact3.png` are labeled contact sheets (round 1 / round 2).

**Terminology (from Anthropic's own code):** the character is simply **Clawd**
(`alt="Clawd"`, `const clawd`, `CLAWD_RENDER_W`, component `ClawdLaptopInner`,
CSS group `group/clawd`) — the word "mascot" never appears. Variants are
organized **core** (plain Clawd) vs **persona** (costume/scene vignettes).
The desk-pet feature is a separate system called the **Hardware Buddy**.

**Unlisted assets:** several files answer at their URL but are referenced by
no shipped bundle (found by name probing — the server serves real GIFs for
valid names and an HTML catch-all otherwise): Soccer, Basketball, Skateboard,
Trumpet (persona); Walking, Jumping, Pointing (core). Presumably staged for
seasonal/promo use — Soccer is the FIFA World Cup 2026 football clip.

## Core set (`/images/clawd/core/`, 2750×1850 canvas, 50 px per art-pixel)

| File | Frames | Timing | Scene / used as |
|---|---|---|---|
| `Clawd-CrabWalking.gif` | 20 | 80 ms | side-walking crab — session working/loading indicator |
| `Clawd-Waving.gif` | 17 | 80 ms | waving hello — welcome/onboarding |
| `Clawd-Walking.gif` | 28 | 90 ms | front-facing stroll (unlisted) |
| `Clawd-Jumping.gif` | 21 | 90 ms | neutral hop (unlisted) |
| `Clawd-JumpingHappy.gif` | 21 | 90 ms | happy-eyes hop — also at `/images/home-page-assets/` |
| `Clawd-Pointing.gif` | 43 | 90 ms | pointing gesture (unlisted) |
| `Clawd-Dancing.gif` | 40 | 90 ms | celebration — `/images/spotlights/claude-code-celebration/` |
| `Clawd-Still.png` | 1 | — | front idle — reduced-motion fallback |
| `Clawd-Laptop.lottie.json` | 43 @ 12 fps | Lottie 5.7.4 | blinks, turns, types on a laptop (`nm: "Clawd-Laptop"`) |
| `Clawd-Soccer.lottie.json` | 61 @ 12 fps | Lottie 5.7.4 | football juggling; layers add `white` (`#F9F8F4`) for the ball; x-offset 486 instead of 736, same 34×23 grid. Recovered from a public archive (official Anthropic export format; the GIF twin is live on claude.ai) |

Lottie source: `https://assets-proxy.anthropic.com/claude-ai/v2/assets/v1/c838f53ee-DqwARLA7.json`
(hash-versioned; the stable reference lives in the desktop app's
`resources/ion-dist/assets/v1/*.js` and the claude.ai/code JS bundles).

### Lottie geometry (decoded)

Pure axis-aligned rects on a 50 px grid: x ≡ 36 (mod 50) starting at 736,
y ≡ 1 (mod 50) starting at 701 → **34×23 cell grid**. Four color layers,
z-order bottom→top: `color_3` (gray, `#8B8B8B`), `dark_orange` (`#BE684D`),
`orange` (`#D87656`), `black`. Each layer holds one shape-group per pose;
a group's fill opacity is keyframed 0/100 to select which frames show it.
Decoding recipe: for frame t, paint every group whose opacity at t is 100.

## Persona set (`/images/clawd/persona/`, 1189×800 canvas)

| File | Frames | Timing | Scene |
|---|---|---|---|
| `Clawd-Cloud-once.gif` | 62 | 80 ms | riding a cloud (plays once as intro) |
| `Clawd-Cloud-still.png` | 1 | — | cloud resting frame |
| `Clawd-Magnifier.gif` | 113 | 80 ms | detective hat + magnifying glass (byte-identical copy at `/images/install-hub/clawd-magnifier.gif`) |
| `Clawd-RacingCar.gif` | 48 | 80 ms | driving a go-kart |
| `Clawd-Soccer.gif` | 61 | 90 ms | juggling a football — FIFA World Cup 2026 (unlisted) |
| `Clawd-Basketball.gif` | 56 | 90 ms | dribbling (unlisted) |
| `Clawd-Skateboard.gif` | 74 | 90 ms | skateboarding (unlisted) |
| `Clawd-Trumpet.gif` | 81 | 90 ms | golden trumpet + floating notes (unlisted) |
| `7bbe5052.gif` | 11 | 160 ms | sailing a boat (short loop) |
| `ac0fa108.gif` | 68 | 80 ms | sailing a boat (full scene) |

## Micro set (Claude Code CLI)

| File | Frames | Timing | Scene |
|---|---|---|---|
| `Clawd-Micro-Painter.gif` | 2 | 420 ms | 16×14 px Clawd writing with a pencil, arm raises — "Waiting for Claude…" indicator |

Source: embedded as ASCII grids (`CLAWD_FRAMES` / `CLAWD_PAL`, function
`drawClawd`) in the CLI binary (`~/.local/share/claude/versions/<v>`, found in
2.1.226). Palette: `O #d97757` body, `D #2a1f1b` eyes, `E #e98fa2` eraser,
`F #7d848a` ferrule, `P #e8b93c` pencil, `W #d9a066` wood, `G #4a4a4a` tip.
The archived GIF here is our 16× nearest-neighbor render of those grids.

**Searched and not found (2026-08-10):** no wizard/magic-themed Clawd exists on
any official surface — probed `Wizard Magic Mage Magician Sorcerer Witch Wand
Spell Potion Crystal CrystalBall Fortune WizardHat Magical` across
`clawd/{persona,core}`, `home-page-assets`, `install-hub` (both cases,
gif+png), plus bundle greps of ion-dist, the CLI (its "wizard" strings are an
MCP setup-wizard feature), docs, and marketing pages. The wizard Clawds on
GitHub (e.g. `AnimatedClawdMascot` variants) are fan-made SVG/CSS art.

Related non-pixel extras: `clawd-guest-pass.svg` (static illustration,
`/images/clawd-guest-pass.svg`), `clawd-laptop.webm`/`.mov` (video renders of
the Laptop Lottie, shipped in the desktop app's `ion-dist/images/install-hub/`),
and a playable canvas mini-game "Clawd's Code Cleanup" at
`/animations/cc-celebration/clawd-game.html` (Cloudflare-gated HTML route;
constants `CLAWD_RENDER_W = 48`, `CLAWD_RENDER_H = 38` in the spotlight bundle).

The two hash-named files are live at those literal paths
(`/images/clawd/persona/7bbe5052.gif` etc.).

Also on the persona canvas despite the old catalog calling it "core":
`Clawd-Lurking.gif` (67 frames, 80 ms, peeking from the edge of promo cards) —
served from `/images/clawd/core/Clawd-Lurking.gif` at 1189×800.

## Palette (from the Lottie; GIFs match)

- body orange: `#D87656` (216,118,86)
- shading dark orange: `#BE684D` (190,104,77)
- gray (steam/motion lines): `#8B8B8B` (139,139,139)
- eyes: `#000000`

## Related: anthropics/claude-desktop-buddy

Official public repo: <https://github.com/anthropics/claude-desktop-buddy> —
"Reference and an example for the Bluetooth API for makers in Claude Cowork &
Claude Code Desktop". Documents a Nordic UART Service wire protocol
(REFERENCE.md), a GIF character-pack format (`manifest.json` mapping seven
states: sleep, idle[], busy, attention, celebrate, dizzy, heart), and a
folder-push transport the desktop app's Hardware Buddy window uses to install
characters onto devices. No Clawd art in the repo (example character is bufo),
but the state model is the official vocabulary for desk-pet behavior.

## Prior art

`companion-inc/pet-clawd` on GitHub archived the same core/persona sets
(2026-06-12) plus trimmed frame exports; its `art/clawd-official/README.md`
documented the in-app usage of each clip.
