# Dashboard UX Task 3 Report

## Status

Implemented semantic Activity rows from base
`64fa3ab9bde05a960ee4e63be3a671ec2864fdb7`. Claude Code now has separate
Open, Busy, and Waiting labels, and Codex has a separate Unread label. The
provider titles remain in the normal foreground color. No payload, daemon,
carousel, Robot-page, COM3, tray, or upload behavior was changed.

## TDD RED

Tests were added before production code.

Focused pytest:

```powershell
& 'C:\Users\Gustavo\Documents\esp32-claude\.venv\Scripts\python.exe' -m pytest tests\test_esp32_2432s024c_contract.py -q
```

- Exit `1`: `2 failed, 21 passed`.
- Failures identified the missing independent Activity widgets and compact/tall
  row layout contract.

Unity compile/link:

```powershell
& 'C:\Users\Gustavo\.platformio\penv\Scripts\pio.exe' test -d firmware -e esp32_2432s024c -f test_port_helpers --without-uploading --without-testing
```

- Exit `1`: compilation stopped because `activity_style.h` did not exist.
- This was the expected missing-production-contract failure.

## Minimal implementation

- Added `activity_style.h` with the approved semantic colors:
  - Open `#55A868`
  - Busy `#D97757`
  - Waiting `#5B8FF9`
  - Codex Unread `#8B7CF6`
- Replaced the two multiline metric labels with four independent LVGL labels.
- Used `L.detail_font` for titles, rows, and provider-unavailable labels.
- Used compact Claude Y positions `20`, `40`, `60` and Codex Y `28` when
  `L.usage_panel_h <= 90`; otherwise used `28`, `52`, `76` and `36`.
- Initialized metric rows hidden and empty. A provider with invalid data shows
  only muted `Unavailable`; it never displays an invented zero.
- A valid provider update hides `Unavailable`, updates each independent string,
  and restores only that provider's metric rows.
- Left Activity freshness application, footer formatting, and periodic refresh
  unchanged.

## GREEN and toolchain investigation

Focused pytest passed with `23 passed in 0.04s`.

The exact default Unity command encountered a reproducible GCC 14.2.0 internal
compiler error at `cfgcleanup.cc:580` while PlatformIO used its implicit 28-job
default. Systematic reproduction failed in different untouched sources on each
run:

1. `lv_draw_sw_blend_to_a8.c`
2. `IPAddress.cpp` and `cbuf.cpp`
3. `lv_draw_sw_box_shadow.c` and `lv_draw_sw_transform.c`

The machine had about 75 GiB free physical memory, so exhaustion was not the
observed cause. A complete portrait build with the same toolchain and `-j 1`
compiled every previously failing source and linked successfully. This isolated
the failure to high-parallelism toolchain execution rather than Task 3 source.

Because `pio test` does not expose a jobs option, the same PlatformIO test
runner and `test_port_helpers` target were then invoked with the Click `jobs`
default set to `1` for that process only:

```powershell
& 'C:\Users\Gustavo\.platformio\penv\Scripts\python.exe' -c "import sys; import platformio.run.cli as run_cli; next(param for param in run_cli.cli.params if param.name == 'jobs').default = 1; sys.argv = ['pio', 'test', '-d', 'firmware', '-e', 'esp32_2432s024c', '-f', 'test_port_helpers', '--without-uploading', '--without-testing']; from platformio.__main__ import main; main()"
```

- Unity target: `PASSED`, exit `0`, compile/link duration `180.18s`.
- `--without-testing` intentionally compiles and links without hardware
  execution, so PlatformIO reports zero executed device test cases.
- The known `_fixdfdi.o` missing `.note.GNU-stack` linker warning remained
  non-fatal.
- No project, dependency, or installed toolchain file was changed to obtain the
  serial result.

## Final verification

```powershell
& 'C:\Users\Gustavo\Documents\esp32-claude\.venv\Scripts\python.exe' -m pytest daemon\tests tests -q
& 'C:\Users\Gustavo\.platformio\penv\Scripts\pio.exe' run -d firmware -e esp32_2432s024c -j 1
& 'C:\Users\Gustavo\.platformio\penv\Scripts\pio.exe' run -d firmware -e esp32_2432s024c_landscape -j 1
```

- Python: `153 passed, 2 skipped, 2 warnings`, exit `0`. The warnings are the
  two known unawaited `Event.wait` coroutine warnings in
  `daemon/tests/test_windows_reconnect.py`.
- Portrait: `SUCCESS`, exit `0`; RAM 32.3% (105,980 bytes), flash 42.4%
  (1,334,531 bytes). A fresh incremental recheck also passed in `28.57s`.
- Landscape: `SUCCESS`, exit `0`, duration `303.13s`; RAM 32.3% (105,980
  bytes), flash 42.4% (1,334,667 bytes).
- Existing non-fatal warnings remained: deprecated `NimBLEService::start()` and
  the `_fixdfdi.o` executable-stack linker warning.

## Self-review

- `git diff --check` passed; only expected Windows LF-to-CRLF notices appeared.
- Changed production scope is limited to `activity_style.h` and Activity
  portions of `ui.cpp`; tests cover color constants, independent widgets,
  compact/tall positions, and hide/show availability behavior.
- Search confirmed the old combined Activity format and old combined widget
  names are absent.
- `activity_footer_label` and Robot code remain present.
- No diff exists under payload, daemon, or carousel files.
- No push, upload, COM3 access, tray access, or hardware test was performed.

## Concern

The source and required builds are green. The remaining concern is environmental:
on this 28-core Windows host, the unmodified `pio test` command can trigger a
GCC 14.2.0 ICE when it compiles the test environment at full implicit
parallelism. Serial execution of the identical Unity target is the reliable
workaround and did not hide a functional compilation or link failure.
