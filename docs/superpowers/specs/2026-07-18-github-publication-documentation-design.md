# GitHub Publication and Documentation Design

**Date:** 2026-07-18

**Status:** Approved design, pending implementation plan

**Target repository:** `Atzingen/Clawdmeter` (public GitHub fork)

## Objective

Publish the tested ESP32 dashboard as a transparent fork of
[`HermannBjorgvin/Clawdmeter`](https://github.com/HermannBjorgvin/Clawdmeter)
and reorganize its documentation so another person can understand the fork,
buy the supported low-cost board, flash it on Windows, and use its Claude Code,
Codex, and Activity pages without reading the source code.

The documentation must distinguish inherited Clawdmeter work from the additions
made in this fork. It must not present internal Codex data formats as a stable or
official API.

## Repository Strategy

Create the public GitHub fork `Atzingen/Clawdmeter` instead of an unrelated new
repository. This preserves GitHub's fork relationship and commit ancestry.

After the fork exists, local remotes will be organized as follows:

- `origin`: `Atzingen/Clawdmeter`, used for pushes.
- `upstream`: `HermannBjorgvin/Clawdmeter`, used for fetching upstream changes.

The verified implementation branch will be published to the fork without
rewriting its history. The fork's default branch will contain the documented,
physically validated version. No pull request to the upstream project is part of
this scope.

## Documentation Structure

### Primary README

`README.md` remains English-first for the existing international audience. Its
opening must immediately state that this repository is a fork and link both the
original project and the Portuguese guide.

The primary README will be reorganized into this order:

1. Project summary and explicit upstream attribution.
2. Visual gallery using repository-owned screenshots and splash animation.
3. "What this fork adds" comparison table.
4. Supported screens and touch behavior.
5. Low-cost ESP32-2432S024C hardware profile and trade-offs.
6. Windows quick start for the tested USB-serial setup.
7. Data sources and privacy/compatibility notes.
8. Build and flash instructions for portrait and landscape.
9. Troubleshooting and links to detailed platform/porting documentation.
10. Credits and the inherited licensing/brand-assets warning.

Existing macOS, Linux, BLE, font, icon, and animation-generation material will
not be deleted. Long platform-specific detail may remain in its existing section
or be linked from the overview, but the tested Windows path must be easy to find.

### Portuguese Guide

Add `docs/README.pt-BR.md` with a practical Portuguese guide. It will cover:

- project origin and differences from upstream;
- exact tested board and USB orientation;
- prerequisites;
- installation of PlatformIO and the Windows daemon;
- portrait and landscape build/flash commands;
- the three pages and touch navigation;
- local data sources for Claude Code and Codex;
- common failures involving COM ports, credentials, unavailable metrics, and
  mirrored/rotated displays;
- purchase/reference links and hardware trade-offs.

The Portuguese guide is not a line-by-line translation. It prioritizes the
tested ESP32-2432S024C workflow.

## Upstream Attribution and Differences

The README must use the name **Clawdmeter**, not speech-recognition variants such
as "Cloud Mirror". It must credit Hermann Bjorgvin and link the original
repository near the top and again in Credits.

The comparison will clearly identify these fork additions:

| Area | Original baseline | This fork |
| --- | --- | --- |
| Providers | Claude Code usage | Claude Code plus Codex local usage/activity |
| Dashboard | Original usage/splash flow | Claude, Codex, and combined Activity pages |
| Navigation | Original screen behavior | Left/right touch navigation, 12-second auto-cycle, 30-second manual holdoff |
| Windows transport | BLE-oriented baseline | Tested USB serial transport and tray app; Bluetooth not required for this board |
| Hardware | Waveshare AMOLED and other upstream ports | ESP32-2432S024C 2.4-inch 240x320 capacitive TFT, portrait and USB-left landscape |
| Activity | Claude-oriented status | Claude Open/Busy/Waiting plus Codex Unread with semantic colors and freshness |

Claims will be phrased against the upstream baseline from which the local work
started, not as a claim that upstream can never gain similar features later.

## Hardware Documentation

The tested board will be documented as **Sunton/Jingcai
ESP32-2432S024C**, 2.4-inch 240x320 capacitive-touch TFT, based on a classic
ESP32-WROOM-class module with Wi-Fi and Bluetooth.

Stable links will include:

- a technical board reference, such as the CircuitPython board page;
- one exact-model retail listing where available;
- a search link for marketplaces whose individual listings change frequently;
- the upstream Waveshare ESP32-S3-Touch-AMOLED-2.16 page for comparison.

The cost advantage will be stated qualitatively: this TFT/classic-ESP32 board is
commonly a lower-cost alternative to the higher-resolution Waveshare AMOLED
board. The documentation will not freeze a price or guarantee that every current
seller is cheaper. It will tell buyers to verify the suffix `C` for capacitive
touch and the exact `ESP32-2432S024C` model.

The trade-off section will state that the lower cost comes with a 240x320 TFT,
less memory/integration, and no equivalent AMOLED resolution, audio subsystem,
IMU, or advanced battery management found on the Waveshare board.

## Claude Code and Codex Data

The Windows daemon behavior will be documented from the implemented code:

- Claude usage percentages come from the existing authenticated Claude polling
  path.
- Claude activity aggregates local session status into Open, Busy, and Waiting.
- Codex usage aggregates local `token_count` and rate-limit events under
  `%USERPROFILE%\.codex\sessions`.
- Codex Unread is read from `%USERPROFILE%\.codex\.codex-global-state.json`.
- Only aggregate values are sent to the ESP32 over USB serial.

The guide must explicitly warn that Codex session/state JSON is a local internal
format, not a documented public OpenAI API. When the expected schema is absent or
changes, Codex fields appear as `Unavailable` while Claude can continue working.

For product context, the documentation may link the official
[`openai/codex`](https://github.com/openai/codex) repository and official Codex
documentation. It must not imply that OpenAI endorses this dashboard.

## Visual Assets

The documentation will reuse existing repository assets rather than inventing
screens that were not photographed:

- `assets/demo.gif` for the animated splash;
- `screenshots/splash.png` and `screenshots/amoled_18/splash.png` for splash
  examples;
- `screenshots/usage.png` and existing demo assets for inherited UI context;
- Claude and OpenAI/Codex marks already used by the firmware, with attribution
  and brand-guideline links where appropriate.

Because there is no camera evidence of the final ESP32-2432S024C dashboard, the
README will not label an inherited AMOLED screenshot as the new TFT result. A
new hardware-photo section will wait until a real device photo is available; no
fabricated hardware photograph or screenshot will be committed.

## Publication Flow

1. Fetch current upstream state and record divergence without rebasing or
   rewriting the physically validated commits.
2. Create the GitHub fork under `Atzingen`.
3. Configure `origin` and make `upstream` fetch-only for normal use.
4. Implement the English README and Portuguese guide on the verified branch.
5. Validate every local link, documented command, environment name, and image
   path; run the existing automated suite because documentation includes command
   and configuration references.
6. Commit the documentation changes with a focused commit.
7. Push the verified branch to the fork's default branch and verify the GitHub
   pages render and are publicly accessible.

## Safety and Compatibility

- Preserve the upstream licensing gray-area warning covering proprietary fonts
  and mascot/brand assets.
- Do not add a new software license that claims rights over inherited assets.
- Do not publish credentials, local logs, `.codex`/`.claude` session contents,
  serial payload captures with private data, or machine-specific paths beyond
  generic examples.
- Do not change firmware or daemon behavior as part of this documentation task.
- Do not claim fixed hardware prices or official support from Anthropic/OpenAI.
- Keep the physically validated landscape firmware and running tray untouched
  during documentation-only work.

## Verification and Success Criteria

The work is complete when all of the following are true:

- `Atzingen/Clawdmeter` exists publicly as a fork of the original repository.
- GitHub shows the expected default branch and the local remotes are unambiguous.
- The README credits upstream before describing fork-specific work.
- English and Portuguese documentation explain installation and everyday use.
- Existing splash assets render in the GitHub README/gallery.
- The board link identifies the exact capacitive `ESP32-2432S024C` model and
  explains cost advantages and trade-offs without a stale fixed-price claim.
- Claude Code, Codex, Activity, USB serial, portrait, landscape, touch, auto-cycle,
  and unavailable-state behavior are documented consistently with the code.
- Automated tests still pass and the worktree is clean.
- The final GitHub repository URL and documentation links return HTTP 200.

## Non-Goals

- Opening or merging an upstream pull request.
- Creating new firmware features, screenshots, enclosure models, or release
  binaries.
- Supporting Codex through an undocumented remote API.
- Replacing upstream assets, fonts, branding, or the splash animation library.
- Guaranteeing marketplace inventory, seller quality, delivery, or price.
