# ESP32-2432S024C Landscape Build Design

Date: 2026-07-16

## Goal

Add a second firmware build for the 2.4-inch ESP32-2432S024C that renders the dashboard at 320x240 with the USB connector on the left. The existing 240x320 portrait build remains available and unchanged as a selectable PlatformIO environment.

The landscape build keeps the four dashboard pages, USB serial transport, touch navigation, 12-second automatic carousel, 30-second manual holdoff, BOOT behavior, orange robot, and current color/orientation corrections.

## Build architecture

Keep `esp32_2432s024c` as the portrait environment and add `esp32_2432s024c_landscape` as an environment that inherits the same board, libraries, source filter, partition layout, and transport settings. The landscape environment adds one compile-time flag, `BOARD_LANDSCAPE`.

The board port will distinguish native panel dimensions from logical UI dimensions:

- native ST7789 panel: 240x320 in both builds;
- portrait logical canvas: 240x320, rotation 0;
- landscape logical canvas: 320x240, rotation 3, with the USB connector physically on the left.

The physical display is always initialized with its native dimensions. Rotation is applied afterward. Flush clipping uses the logical dimensions.

## Touch mapping

Portrait keeps the existing raw-to-portrait mapping. Landscape adds a dedicated raw-to-landscape transform selected by `BOARD_LANDSCAPE`. It must map the same physical point to the corresponding 320x240 LVGL coordinate after display rotation.

Hardware acceptance is authoritative. It established that USB-left landscape uses ST7789 rotation 3, BGR MADCTL `0x68` (`MX | MV | BGR`), and the CST820 transform `(x, y) = (raw_y, 239 - raw_x)`. The raw center `(120, 160)` maps to logical `(160, 119)`, and touches at all four corners and the center land in the matching logical regions. A short touch anywhere advances exactly one page. These corrections remain isolated to the board-local rotation constant, color-order helper, and touch transform; shared carousel code is unchanged.

## Landscape layout

The 320x240 profile is a distinct layout profile rather than the existing `height <= 320` portrait profile.

### Metric pages

- Header content occupies y=6..46.
- Logo uses a 40x40 box at x=10, y=6.
- Page title uses the small title font and is centered without entering the logo safety area; there must be at least 4 px between rendered logo and title bounds.
- The two usage cards are side by side: x=10 and x=165, y=52, width=145, height=126, with a 10 px gap.
- Claude and Codex use one card per usage window. Existing fallback behavior remains: Codex with one window uses the second card for `Tokens today`; with no windows it shows `No limit data` plus daily tokens.
- Activity uses the same two-card grid: Claude Code on the left and Codex on the right. `Waiting` and `Unread` remain separate concepts, and valid zero remains `0`.
- Footer status is centered at y=196 and must end above the indicators.
- Four 5 px page dots remain centered at y=227 with an 8 px gap. They are hidden on Robot.

### Robot page

The robot remains orange, unmirrored, and centered on the 320x240 canvas. Its sprite and status label are scaled/positioned so neither clips. The page dots and metric logo remain hidden.

### Portrait hardening

The existing 240x320 profile stays visually equivalent, but its small-display title bounds are corrected so `Activity` cannot overlap the logo. A geometry test must require a real gap rather than only checking vertical fit.

## Provider freshness and unavailability

The final review found that a local-only v2 payload could make old Claude data appear freshly updated and that a previously valid local provider could remain visible after its collector became unavailable. This is corrected as part of the shared work because both orientations use the same data model.

- `ui_update` receives the parser update mask.
- A provider timestamp/validity changes only when its corresponding update bit is present.
- A v2 payload containing only Codex or Activity data never refreshes Claude freshness.
- The daemon emits explicit compact tombstones for attempted local collectors: `x: null` for unavailable Codex usage and `a.cl: null` / `a.cx: null` for unavailable activity providers.
- The parser sets the provider update bit for both a valid object and an explicit tombstone. A tombstone clears only that provider's validity and renders `Unavailable`.
- An omitted provider means “not updated” and preserves its previous state without renewing freshness.
- v1 payload compatibility remains unchanged.

No prompt, response, filesystem path, session name, or task title may enter the serial payload.

## Error handling

- Invalid or oversized JSON continues to be rejected without changing the current page.
- Unknown local Codex schemas produce the explicit unavailable state while Claude continues operating.
- Zero token counts and zero activity counts are valid data, not unavailability.
- Display or touch rotation is isolated to the ESP32-2432S024C board port.

## Verification

Automated verification will cover:

- portrait and landscape layout metrics and non-overlap bounds;
- native versus logical display dimensions;
- landscape touch mapping at four corners and center;
- portrait and landscape firmware builds;
- sequential payloads: full data followed by local-only data must not refresh Claude;
- valid provider followed by a tombstone must render unavailable;
- zero-token and zero-count values remain valid;
- v1 payload compatibility and compact payload size;
- privacy sentinels for session name, title, prompt, response, and path;
- existing carousel order, 12-second interval, 30-second holdoff, and foreground touch layer.

Physical acceptance on COM3 will verify:

1. USB is on the left and the image is upright and unmirrored.
2. Touch corners and center match the visible orientation.
3. Claude, Codex, Activity, and Robot fit without clipping or overlap.
4. No battery indicator appears.
5. Automatic and manual page transitions retain their existing timing.
6. The production landscape firmware receives repeated positive USB serial acknowledgements.

After landscape acceptance, the portrait environment remains buildable and can be flashed again without reverting source changes.

## Out of scope

- Runtime orientation switching.
- IMU-based rotation.
- Persisting orientation in device settings.
- Changing transport from USB serial.
- Redesigning metrics, colors, carousel timing, or robot artwork beyond the positioning required for 320x240.
