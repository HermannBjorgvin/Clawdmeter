# ChatGPT pet sprites — sourcing and layout

Context for future sessions. Goal: **ship a ChatGPT pet as the mascot art for
Petmeter's Codex mode**, the counterpart to `research/clawd-official/` on the
Claude side. The conversion pipeline is
`tools/convert_pet_spritesheet.py`; it emits `firmware/src/pet_animations.h`
in the same `splash_anim_def_t` format the Clawd art uses.

## Where the asset came from

`codey-spritesheet.webp` — the Codey sprite sheet, obtained by the repo owner
from OpenAI's pets documentation, <https://learn.chatgpt.com/docs/pets?surface=app>.
It matches the sheet format that page documents for **custom** pets:
transparent PNG or WebP, exactly 1536 x 1872.

## Licensing

Codey is OpenAI's artwork and carries no license grant, exactly as the Clawd
art in `research/clawd-official/` is Anthropic's. See the root `README.md`
licensing warning — this repository is deliberately unlicensed because it
bundles proprietary fonts and mascot art, and this asset adds a second vendor
to that position rather than resolving it.

The pets docs describe **custom** pets as user-supplied sheets. Swapping this
file for a commissioned or self-made sheet at the same dimensions is a
drop-in change: the converter reads the grid, not the character. That is the
route to a version of this firmware that could actually carry a license.

## Sheet layout

8 x 9 grid of 192 x 208 frames. Rows are ragged — occupancy is detected from
alpha rather than assumed:

| Row | Frames | Name       | Reading of the art       |
|-----|--------|------------|--------------------------|
| 0   | 6      | `idle`     | stands, blinks           |
| 1   | 8      | `walking`  |                          |
| 2   | 8      | `running`  |                          |
| 3   | 4      | `waving`   |                          |
| 4   | 5      | `jumping`  |                          |
| 5   | 8      | `dizzy`    | `x_x` / `>_<` — "blocked" |
| 6   | 6      | `thinking` | hand to chin             |
| 7   | 6      | `laptop`   | reads as "running work"  |
| 8   | 6      | `happy`    | `^_^` — "ready"          |

57 frames total. The docs name four pet states — Running, Needs input, Ready,
Blocked — but the sheet does not label its rows, so the mapping above is a
reading of the artwork, not a declared contract. Revisit it if OpenAI
documents the row order.

## Why this converter differs from the Clawd one

The Clawd assets are true pixel art on an integer grid, so
`convert_official_clawd.js` recovers cells by sampling cell centers after
fitting the grid phase. These sprites are anti-aliased with soft shading and
drop shadows — a run-length scan over the opaque pixels returns gcd 1, so
there is no native pixel grid to recover. Cells are area-downsampled and then
quantized instead.

Every frame is cropped against ONE global content bounding box rather than
per-frame boxes, which is what keeps the character anchored across
animations — otherwise switching from idle to walking makes it hop.
