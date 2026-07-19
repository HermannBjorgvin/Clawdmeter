# Claude Fable Three-Bar Dashboard Design

**Date:** 2026-07-19
**Status:** Approved for implementation planning

## Goal

Add the real Claude Fable weekly scoped allowance to the Claude dashboard and reorganize the ESP32-2432S024C landscape screen so `Currently`, `Weekly`, and `Fable` remain readable at 320x240. Remove the obsolete lower status strip that visually conflicts with the three-page indicator and retain its rotating word as a small companion to the indicator.

## Scope

This change covers:

- collecting the Fable scoped weekly percentage and reset time on the host;
- transporting optional Fable fields in the existing compact JSON payload;
- parsing those fields without breaking older payloads or firmware;
- rendering three compact usage rows on the 320x240 Claude page;
- replacing the existing large animated footer with a small word beside the page dots;
- automated tests, firmware build, upload, and physical display validation.

The Codex and Activity pages, page order, automatic rotation, and left/right touch navigation remain unchanged. The selected visual rearrangement applies only to the 320x240 landscape layout. Existing layouts for other display sizes remain unchanged in this iteration.

## Data Source

The host reads Fable usage from the authenticated Claude OAuth usage endpoint:

```text
GET https://api.anthropic.com/api/oauth/usage
anthropic-beta: oauth-2025-04-20
Authorization: Bearer <existing Claude token>
```

The collector selects an active item from `limits` with:

- `kind == "weekly_scoped"`;
- `scope.model.display_name == "Fable"`;
- numeric `percent`;
- a parseable `resets_at` value when available.

The endpoint is an internal Claude Code OAuth interface rather than a documented public telemetry API. Parsing therefore remains defensive. Missing, malformed, inactive, or renamed scoped limits produce an unavailable Fable state instead of a fabricated zero.

The host caches a successful Fable result for 180 seconds to avoid querying the usage endpoint on every 60-second device update. A failed refresh does not overwrite a still-valid cached result. Once no valid result is available, the outgoing payload omits the Fable fields.

## Payload Contract

The existing top-level Claude payload gains two optional compact keys:

| Key | Meaning | Type |
| --- | --- | --- |
| `f` | Fable weekly scoped usage percentage, clamped to 0-100 | integer |
| `fr` | Minutes until the Fable scoped limit resets | integer |

Both keys are omitted when Fable usage is unavailable. Their absence is the validity signal, so `f: 0` remains a valid measured value.

Older firmware ignores the new keys. New firmware accepts payloads without them and shows Fable as unavailable. This preserves compatibility in both directions.

## 320x240 Claude Layout

The selected layout is **three full-width horizontal rows**.

- The content remains below the existing Claude title and provider mark.
- `Currently`, `Weekly`, and `Fable` receive equal visual weight.
- Each row contains the metric name, compact reset text, percentage, and a thin progress bar.
- Progress bars are 8 pixels high instead of the current 24 pixels.
- All three bars retain the existing percentage thresholds: green below 50%, amber from 50% through 79%, and red from 80% onward.
- Reset text may use compact forms such as `2h 14m` or `1d 7h`, without the longer `Resets in` prefix when space is constrained.
- If Fable is unavailable, its row shows `Unavailable`, keeps the empty track visible, and does not display `0%`.

The compact rows start at y=52, use a 47-pixel height and a 5-pixel gap, and therefore occupy y=52 through y=203. Dedicated compact-row metrics are added to `UiLayoutMetrics`; Codex and Activity card geometry is not altered.

## Footer and Page Indicator

The old Claude status label and its decorative `glyph + word + ellipsis` presentation are removed from their current full-width footer position.

The page indicator remains three dots. A small status word appears immediately to the right of the dots on the Claude page only:

- use the existing small 14-pixel display font;
- show only the rotating word, for example `Working`;
- omit the spinner glyph and trailing ellipsis;
- clip unusually long words rather than shifting or covering the dots;
- do not render a separator line or underline.

Waiting, connecting, connected, updated, listening, and no-data states continue to use their existing state-selection logic; only their compact presentation changes.

## Error Handling

- OAuth authentication failures continue through the tray's existing Claude login error path.
- A Fable usage endpoint failure does not block the regular Claude, Codex, or Activity update.
- Invalid percentages, reset timestamps, or unexpected JSON shapes make only Fable unavailable.
- The firmware clamps accepted percentages to 0-100 and treats a missing reset as unknown.
- The serial acknowledgement and reconnect behavior is unchanged.

## Verification

Host tests must cover:

- selection of the exact active Fable `weekly_scoped` limit;
- preservation of a valid measured `0%`;
- reset timestamp conversion;
- missing, inactive, malformed, and non-Fable scoped limits;
- cache behavior and independent failure from the regular Claude poll;
- payload inclusion and omission of `f` and `fr`.

Firmware tests must cover:

- parsing optional Fable fields;
- compatibility with old payloads;
- valid zero versus unavailable state;
- three compact rows fitting above the footer on 320x240;
- thin bar geometry;
- no overlap between the status word and three page dots;
- unchanged Codex and Activity geometry and carousel behavior.

Completion requires a successful host test suite, PlatformIO native tests, ESP32-2432S024C firmware build, upload to the connected board, serial acknowledgement, and visual inspection of the physical display.
