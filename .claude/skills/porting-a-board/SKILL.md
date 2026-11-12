---
name: porting-a-board
description: Adds a new display board (ESP32 dev kit) to Clawdmeter end to end - firmware board folder from the template, PlatformIO env, BoardCaps, the board's CLAUDE.md, desktop SVG art, boards.rs / ui/mod.rs / README / release-manifest entries, and the PR requirements (buy link, photo, hardware testing). Use when asked to port, add or support a new board, panel or dev kit, or to finish or review a board-port PR.
---

# Porting a board

The guide is [docs/porting/adding-a-board.md](../../../docs/porting/adding-a-board.md):
read it first. It has the reference-board table, the implementation steps and a
symptom → check table for black screens, edge strips, touch offsets and
brownouts. The HAL each board implements is in
[docs/porting/hal-contract.md](../../../docs/porting/hal-contract.md); optional
hardware is declared per [docs/porting/capability-flags.md](../../../docs/porting/capability-flags.md).

Before writing code, search for an existing PR for the board
(`gh pr list --state all --search "<board name>"`). Testing someone's port
helps more than a duplicate.

## Checklist

Copy this and tick it off. Every item is part of the port, not follow-up work.

```
Port progress:
- [ ] Reference board chosen (same SoC and display bus)
- [ ] firmware/src/boards/<env>/ copied from boards/template and filled in
- [ ] [env:<env>] in firmware/platformio.ini, and <env> added to default_envs
- [ ] firmware/src/boards/<env>/CLAUDE.md with pins and hardware gotchas
- [ ] Row for the board in the root CLAUDE.md "Boards" table
- [ ] caps.cpp: round_panel = true if the glass is round; no layout code anywhere
- [ ] Panel size in PANELS (firmware/test/test_layout) if it's a new shape
- [ ] pio run -d firmware -e <env> builds; pio test -d firmware -e native passes
- [ ] desktop/crates/clawdmeter-app/assets/boards/<env>.svg (original art)
- [ ] boards.rs entry + SVG registered in ui/mod.rs; cargo test -p clawdmeter-app boards passes
- [ ] README "Supported boards" row
- [ ] DISPLAY_NAMES entry in tools/ci/make_manifest.py; python3 -m unittest discover -s tools/ci/tests passes
- [ ] UI checked at the panel's native size (verifying-firmware-ui skill)
- [ ] PR body: buy link, photo or "no photo", exact hardware tested
```

## Firmware

- Copy the closest PlatformIO env. Keep the shared build settings, exclude
  `boards/` in `build_src_filter`, then include only the new folder. Pick flash
  size, partitions and PSRAM settings from the real hardware; never copy S3
  PSRAM settings into a C6 env. CI finds hardware envs by itself
  (`tools/ci/hardware_envs.py`), so no workflow edit is needed.
- Shared code never gets `#ifdef BOARD_*`. If the board needs different
  behaviour, add a `BoardCaps` field or extend the HAL.
- The meters layout comes from `BoardCaps` width, height and `round_panel`
  (`layout.cpp`); a port never edits `ui.cpp` or `layout.cpp` for its size.
  Add a new panel shape to `PANELS` in `firmware/test/test_layout` and run
  it; if something fails, fix it with a shape rule (narrow, short, round …),
  never a board check. See "The meters layout" in the porting guide.
- `board_init()` must release reset lines, power rails and IO expanders before
  `display_hal_init()` / `touch_hal_init()`, or the panel and touch silently
  fail to probe.
- Read `firmware/CLAUDE.md` "Critical gotchas" before debugging a dark panel,
  edge strip or wrong touch coordinates: most of them came from ports.
- The board's `CLAUDE.md` holds what isn't obvious from `board.h`: pins that
  differ from the vendor wiki, required init order, column offsets, button
  edges. Use an existing board's file as the model
  (`firmware/src/boards/waveshare_amoled_18/CLAUDE.md`).

## Desktop app, README and release manifest

- Art: an original SVG at `desktop/crates/clawdmeter-app/assets/boards/<env>.svg`,
  a 3/4 view drawn from the real product and matching the existing
  illustrations. Product photos are reference material, never artwork to copy
  or embed.
- `desktop/crates/clawdmeter-app/src/boards.rs`: display name and buy link.
  Register the SVG in `desktop/crates/clawdmeter-app/src/ui/mod.rs`.
  `boards::tests` fails if an env, its art or its shop link is missing.
- README **Supported boards** table: the same display name and buy link, in
  the **Experimental** table. A board moves to Stable once someone reports it
  working in the "Board support reports" discussion (env name, firmware
  version, a photo).
- `tools/ci/make_manifest.py` `DISPLAY_NAMES`: without it the release ships the
  board under its env name and prints a warning.
- Buy links: Waveshare links carry `?aff_id=149786` (`&aff_id=149786` when the
  URL already has a query); other vendors use a plain product link to the
  maker's own store, or a marketplace listing (Amazon, AliExpress) when the
  maker has none. Same link in the app and the README.

## Verifying

Simulator captures at the panel size are a first pass, not hardware testing
(`verifying-firmware-ui` skill). On hardware, check touch coordinates, every
button, battery state, Bluetooth pairing, brightness, and audio/rotation if
the board has them (`flashing-and-probing-boards` skill). Only flash when the
board is attached and the user has asked for it.

## Pull request

Use `.github/pull_request_template.md` and fill its **New board** section:

- the buy link;
- preferably a photo of the actual board running Clawdmeter, or say none is
  available;
- **Hardware testing**: the exact board and revision and what was verified,
  or "not tested on hardware";
- **Human involvement**: only what the human confirmed they did.

Its **Agent review** section follows the rule in CLAUDE.md's Pull requests
section.
