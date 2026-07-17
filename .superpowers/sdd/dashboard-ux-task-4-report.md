# Dashboard UX Task 4 Report

## Status

Implemented the three-page dashboard integration from base
`2279b548c8871ccb6d9fceb710bfb3a7fb80954c`. The carousel now contains exactly
Claude, Codex, and Activity; short taps route by logical X coordinate in both
directions; automatic navigation remains forward-only at 12 seconds; and both
manual directions retain the 30-second holdoff.

Robot is no longer a dashboard enum value, screen, status label, tick path,
transport-freshness copy, battery special case, branding fallback, startup
sentinel, or power-button condition. The splash subsystem remains available for
boot and the embedded idle creature, independent of dashboard page state.

## TDD RED

Tests were changed before production code.

The first focused contract run reported `3 failed, 23 passed`:

- `DASHBOARD_ROBOT` was still present in `dashboard_carousel.h`.
- Robot status and transport-freshness state were still present in `ui.cpp`.
- `global_click_cb` did not yet read an LVGL input point or call both manual
  directions.

The required PlatformIO Unity command compiled and linked successfully, but
`--without-testing` intentionally executes zero assertions. A temporary host
Unity runner using the real `dashboard_carousel.h` therefore exercised the two
new carousel tests and reported `2 Tests, 1 Failure`: expected page count 3,
actual 4. After implementation and recompilation, the same runner reported
`2 Tests, 0 Failures`. The temporary source, object, and executable were then
removed.

An integration search found two additional production references in
`main.cpp`. A new boot-splash contract was added first and reported
`1 failed, 26 passed` because `DASHBOARD_ROBOT` still acted as the startup
splash sentinel. This established the need to separate boot splash visibility
from dashboard state before changing `main.cpp`.

## Implementation

- Removed `DASHBOARD_ROBOT`; `DASHBOARD_PAGE_COUNT` is now 3, so the existing
  indicator loop creates exactly three dots.
- Kept the page order Claude -> Codex -> Activity -> Claude and reverse wrap
  Claude -> Activity -> Codex -> Claude.
- Replaced the click callback with `lv_indev_active()` and
  `lv_indev_get_point()`, clamped negative X to zero, and classified against the
  logical `L.scr_w`: left half goes previous, center/right goes next.
- Kept the transparent navigation layer full-screen and returned it to the
  foreground whenever a dashboard screen is shown.
- Removed the Robot label, update timestamps, transport copy, one-second Robot
  tick, Robot switch case, indicator hiding, and battery special case.
- Added `ui_show_boot_splash()` so startup hides dashboard containers, logos,
  battery, dots, and navigation before showing the existing splash. The first
  payload still calls `ui_start_dashboard()`, which hides the splash and enables
  the three-page dashboard.
- Changed the power-button splash check to `splash_is_active()` rather than a
  dashboard enum value. `splash_tick()`, pairing behavior, the Claude idle
  creature, Activity freshness, provider branding, battery policy, payload, and
  daemon code were otherwise left intact.

`dashboard_branding.h`, `main.cpp`, and `ui.h` were necessary integration files
not listed in the brief's initial file table: the first referenced the removed
enum, while the latter two preserved boot splash behavior without using a Robot
dashboard page.

## Final verification

Focused contracts:

```powershell
& 'C:\Users\Gustavo\Documents\esp32-claude\.venv\Scripts\python.exe' -m pytest tests\test_esp32_2432s024c_contract.py -q
```

- `27 passed in 0.05s`, exit 0.

Unity compile/link:

```powershell
& 'C:\Users\Gustavo\.platformio\penv\Scripts\pio.exe' test -d firmware -e esp32_2432s024c -f test_port_helpers --without-uploading --without-testing
```

- `esp32_2432s024c:test_port_helpers [PASSED]`, 34.29 seconds, exit 0.
- The parallel GCC ICE did not recur, so the serial test-runner workaround was
  not needed.
- As requested, no upload or physical Unity execution occurred.

Full Python suite:

```powershell
& 'C:\Users\Gustavo\Documents\esp32-claude\.venv\Scripts\python.exe' -m pytest daemon\tests tests -q
```

- `157 passed, 2 skipped`, exit 0.
- Only the known unawaited `Event.wait` coroutine warnings appeared.

Firmware builds:

```powershell
& 'C:\Users\Gustavo\.platformio\penv\Scripts\pio.exe' run -d firmware -e esp32_2432s024c -j 1
& 'C:\Users\Gustavo\.platformio\penv\Scripts\pio.exe' run -d firmware -e esp32_2432s024c_landscape -j 1
```

- Portrait: `SUCCESS`, 254.20 seconds; RAM 32.3% (105,964 bytes), flash 42.4%
  (1,334,379 bytes).
- Landscape: `SUCCESS`, 41.53 seconds; RAM 32.3% (105,964 bytes), flash 42.4%
  (1,334,515 bytes).
- Both retained the known non-fatal `_fixdfdi.o` executable-stack linker
  warning.

## Self-review

- `git diff --check` exited 0; only Windows LF-to-CRLF notices appeared.
- Production search found no `DASHBOARD_ROBOT`, `robot_status_label`, Robot
  refresh timestamp, or copied transport-freshness state.
- Constants remain exactly 12,000 ms automatic advance and 30,000 ms manual
  holdoff.
- The full-screen navigation layer remains transparent, clickable, and brought
  to the foreground for dashboard pages.
- No daemon or payload diff exists.
- No COM3, tray, flash, upload, or push action was performed.

## Post-review correction: battery refresh during boot splash

Global review found a dynamic visibility gap after the Task 4 commit. The boot
helper hid `battery_img`, but `current_page` remained `DASHBOARD_CLAUDE` while
the splash was active. On boards with a battery, a later battery percentage or
charging change called `ui_update_battery()`, which reapplied the page-only
policy and made the battery visible over the splash.

The root cause was that dashboard visibility had been inferred solely from the
current carousel page. The correction adds a zero-initialized
`DashboardVisibilityState`, marks it inactive before showing the boot splash,
marks it active when any dashboard page is shown, and requires both an active
dashboard and a non-Activity page before the battery can be visible. This also
keeps the first `ui_update_battery()` in setup hidden before
`ui_show_boot_splash()` runs.

TDD RED evidence:

- Focused contracts: `2 failed, 26 passed`. The failures showed the old
  page-only battery policy and missing dynamic transition wiring.
- Serial Unity compile/link reached the new pure test and failed on the missing
  `DashboardVisibilityState` and transition helpers, as expected. The
  PlatformIO Python wrapper returned process code 0 despite reporting the
  target `ERRORED`; the compiler diagnostics and target status were used as the
  RED evidence.

GREEN evidence:

- Focused contracts: `28 passed in 0.04s`.
- A temporary host Unity runner using the production header executed the full
  sequence inactive splash -> Claude/Codex dashboard -> Activity -> splash and
  reported `1 Tests, 0 Failures`. Temporary artifacts were removed.
- Serial firmware Unity compile/link: `test_port_helpers [PASSED]` in 35.60
  seconds. No parallel ICE occurred because the runner was forced to one job.
- Full Python suite: `158 passed, 2 skipped`; only the known unawaited
  `Event.wait` warning appeared.
- Portrait build: `SUCCESS` in 252.50 seconds; RAM 32.3% (105,964 bytes), flash
  42.4% (1,334,415 bytes).
- Landscape build: `SUCCESS` in 39.44 seconds; RAM 32.3% (105,964 bytes), flash
  42.4% (1,334,551 bytes).
- Both builds retained only the known non-fatal `_fixdfdi.o` linker warning.
- No payload, daemon, splash animation, pairing, idle, COM3, tray, flash,
  upload, or push behavior was changed or invoked.

## Remaining concern

The automated contracts, Unity compile/link, and both firmware builds are green.
Because hardware access and flashing were explicitly out of scope, physical
touch routing and the visual boot-to-dashboard transition remain hardware
acceptance items; this report does not claim they were observed on-device.
