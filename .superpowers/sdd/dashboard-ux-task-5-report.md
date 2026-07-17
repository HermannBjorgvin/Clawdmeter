# Dashboard UX Task 5 physical validation report

Date: 2026-07-17 (America/Sao_Paulo)

Status: **DONE_WITH_VISUAL_LIMITATIONS**

The definitive landscape firmware is running on the ESP32-2432S024C connected
as COM3. Fresh Python tests, physical Unity, portrait and landscape builds, the
four-image conservative flash, and two post-flash serial update cycles all
passed. No production or test source was changed. The only unobserved items are
the visual appearance and direct manual touch gestures, because this execution
had no camera feed or operator interaction.

## Baseline and review gate

- Worktree: `C:\Users\Gustavo\Documents\esp32-claude\.worktrees\multi-screen-carousel`
- Branch: `codex/multi-screen-carousel`
- Starting HEAD: `f14085064de437e677e03fa9525cba2a386d81e8`
- The linked-worktree check showed a worktree-specific git dir and the common
  repository git dir; it was not a submodule.
- Initial `git status --short`: empty.
- Initial `git diff --check`: exit 0.
- The coordinator supplied the completed global review gate as spec **PASS**,
  quality **APPROVED**, release **Ready YES**, with zero Critical and zero
  Important findings. Hardware access began only after that gate.
- The authoritative brief, design, implementation plan, Tasks 1-4 reports,
  prior landscape physical report, and landscape final-fix report were read
  before execution.
- No push was performed.

## Fresh Python verification

Command:

```powershell
& 'C:\Users\Gustavo\Documents\esp32-claude\.venv\Scripts\python.exe' -m pytest daemon\tests tests -q
```

Result: exit 0; `158 passed, 2 skipped, 1 warning in 0.50s`.

The warning is the existing unawaited `Event.wait` warning in
`daemon/tests/test_windows_reconnect.py::test_start_notify_oserror_does_not_crash_connect_and_run`.

The same full command was rerun immediately before staging the report. That
fresh completion gate also exited 0 with `158 passed, 2 skipped, 1 warning in
0.53s`.

## COM3 identity and tray ownership before Unity

Current Windows PnP identity:

- Name: `USB-SERIAL CH340 (COM3)`
- Manufacturer: `wch.cn`
- VID/PID: `1A86:7523`
- Status: `OK`

The only target process tree was revalidated immediately before termination:

| PID | PPID | Executable | Command | cwd |
|---:|---:|---|---|---|
| 65512 | 68220 | project `.venv\Scripts\pythonw.exe` | `-m daemon.tray_windows` | feature worktree |
| 60868 | 65512 | `C:\Users\Gustavo\miniconda3\pythonw.exe` | `-m daemon.tray_windows` | feature worktree |

The root/child relationship, both command lines, both executable names, and
both cwd values were checked with CIM plus `psutil`. Only PIDs 60868 and 65512
were terminated. The termination script exited 0 and a subsequent CIM query
found zero remaining target PIDs.

A corrected pyserial probe then opened COM3 at 115200 and closed it normally,
exit 0.

### Diagnostic failures retained as evidence

These did not touch source or hardware state beyond the separately successful
tray termination:

1. The project venv process-inspection attempt exited nonzero because that
   interpreter does not contain `psutil` (`ModuleNotFoundError`). The same
   read-only inspection was rerun with Miniconda Python.
2. A broad Miniconda `psutil.process_iter()` inspection timed out before
   returning data. Inspection was narrowed to the two CIM-selected PIDs.
3. The first post-termination pyserial probe script had an unterminated
   ephemeral f-string and exited 1 before opening COM3. The corrected probe
   immediately exited 0 and reported `serial_probe_open=True`.

## Physical Unity

### Standard command and preserved ICE

Command:

```powershell
& 'C:\Users\Gustavo\.platformio\penv\Scripts\pio.exe' test -d firmware -e esp32_2432s024c -f test_port_helpers --upload-port COM3
```

Result: exit 1 before upload; `0` cases executed. GCC 14.2.0 raised the known
high-parallelism internal compiler error in untouched LVGL source:

```text
during RTL pass: cse1
lv_draw_sw_blend_to_i1.c:961:1: internal compiler error:
in try_forward_edges, at cfgcleanup.cc:580
```

This was not treated as a passing gate and was not masked.

### Documented serial fallback

The previously documented conservative runner was used without modifying the
project: it set PlatformIO's per-process `jobs` default to 1 and invoked the
same physical test target and COM3 upload.

Result: exit 0; target `PASSED`; `38 test cases: 38 succeeded` in
`00:03:14.791` (target time 194.79 s).

The hardware-executed cases include:

- exactly three pages and forward/reverse wrap;
- left/right midpoint selection;
- 12-second automatic advance and 30-second manual holdoff;
- page-aware Claude/Codex branding and splash/dashboard battery visibility;
- semantic Activity colors;
- portrait and landscape touch transforms and clamping;
- portrait and 320x240 layout/logo bounds;
- payload compatibility, tombstones, freshness, and aggregate-only state.

The existing `_fixdfdi.o` `.note.GNU-stack` linker warning remained non-fatal.

## Definitive builds after physical Unity

Portrait was built first; landscape was rebuilt last so the definitive flash
used the final landscape artifact.

### Portrait

```powershell
& 'C:\Users\Gustavo\.platformio\penv\Scripts\pio.exe' run -d firmware -e esp32_2432s024c -j 1
```

- Exit 0, `SUCCESS`, 246.96 s.
- RAM: 105,964 / 327,680 bytes (32.3%).
- Flash: 1,334,415 / 3,145,728 bytes (42.4%).

### Landscape final

```powershell
& 'C:\Users\Gustavo\.platformio\penv\Scripts\pio.exe' run -d firmware -e esp32_2432s024c_landscape -j 1
```

- Exit 0, `SUCCESS`, 29.48 s.
- RAM: 105,964 / 327,680 bytes (32.3%).
- Flash: 1,334,551 / 3,145,728 bytes (42.4%).

## Conservative landscape flash

The definitive images were hashed before upload:

| Offset | Image | Bytes | SHA-256 |
|---|---|---:|---|
| `0x1000` | landscape `bootloader.bin` | 23,552 | `A227E3AB93F15F1155EFB3144810CC06FF7A259E9BC9B4542417AFCC6C238214` |
| `0x8000` | landscape `partitions.bin` | 3,072 | `AAAE2888C5A6A348004B5B436F47ABB25AE32E72D9003902955A998EDA723EDD` |
| `0xe000` | framework `boot_app0.bin` | 8,192 | `F94C5D786A7A8FAB06AC5D10E33BF37711A6697636DC037559EA19CC410A17F0` |
| `0x10000` | landscape `firmware.bin` | 1,334,960 | `BB0360B06739F5D4BC34E5072BDBA506571320F7F59BF0C39E06CC1C7C825A14` |

Command, with `PYTHONUTF8=1` and `PYTHONIOENCODING=utf-8`:

```powershell
& 'C:\Users\Gustavo\.platformio\penv\Scripts\esptool.exe' `
  --chip esp32 --port COM3 --baud 115200 `
  --before default-reset --after hard-reset --no-stub `
  write-flash --no-compress --flash-mode dio --flash-freq 40m --flash-size detect `
  0x1000 firmware\.pio\build\esp32_2432s024c_landscape\bootloader.bin `
  0x8000 firmware\.pio\build\esp32_2432s024c_landscape\partitions.bin `
  0xe000 C:\Users\Gustavo\.platformio\packages\framework-arduinoespressif32\tools\partitions\boot_app0.bin `
  0x10000 firmware\.pio\build\esp32_2432s024c_landscape\firmware.bin
```

Result: exit 0 in 144.2 s.

- Connected chip: `ESP32-D0WD-V3`, revision `v3.1`.
- Flash: 4 MB.
- MAC: `a8:42:e3:a8:5a:38`.
- Exactly four `Hash of data verified` lines were emitted, one per offset.
- Final line: `Hard resetting via RTS pin`.

No normal compressed production upload was attempted after the Unity run; the
required conservative writer was used directly.

## Hidden tray and post-flash ACK evidence

The tray was started hidden at `2026-07-17 02:11:57.814 -03:00` with:

```text
C:\Users\Gustavo\Documents\esp32-claude\.venv\Scripts\pythonw.exe -m daemon.tray_windows
```

and the feature worktree as its working directory.

Final process tree:

| PID | PPID | Executable | Command | cwd | State |
|---:|---:|---|---|---|---|
| 70300 | 73076 | project `.venv\Scripts\pythonw.exe` | `-m daemon.tray_windows` | feature worktree | running |
| 58444 | 70300 | `C:\Users\Gustavo\miniconda3\pythonw.exe` | `-m daemon.tray_windows` | feature worktree | running |

The first operational window, measured before the final pytest rerun, used the
pre-launch offset 315,943:

```text
2026-07-17 02:11:58 Clawdmeter identified on COM3
2026-07-17 02:12:00 Sending by USB serial: ...
2026-07-17 02:13:02 Sending by USB serial: ...
```

That first window, after waiting beyond the second 2-second ACK timeout, had:

- `Clawdmeter identified on COM3`: 1.
- `Sending by USB serial`: 2, separated by 62 seconds.
- `USB serial acknowledgement failed`: 0.
- `USB serial write failed`: 0.
- Generic `write fail` lines: 0.
- Both PIDs remained alive with the expected parentage, command lines, and cwd.

The later final pytest rerun writes its mocked Windows/BLE diagnostics to this
same production log as an import side effect. It added clearly synthetic lines
at 02:14:54-55 (`AA:BB:CC:DD:EE:FF`, `API ... mocked`, and test payloads),
including mocked BLE write failures. Those were not tray serial failures. To
avoid relying on attribution alone, a new clean observation window was marked
after pytest at byte offset 321,864. No further test command was run. That
final clean window contained only:

```text
2026-07-17 02:16:06 Sending by USB serial: ...
2026-07-17 02:17:07 Sending by USB serial: ...
```

The two final clean sends were separated by 61 seconds. After waiting beyond
the second ACK timeout, this window had 0 `acknowledgement failed`, 0
`USB serial write failed`, and 0 generic `write fail` lines.

`SerialSession.write_payload()` logs each send before waiting up to two seconds
for a JSON response containing `"ack": true`; any negative/absent ACK causes the
failure line and reconnect. The first subsequent poll plus the post-second
timeout window, with zero failure lines, establishes two positive update
cycles.

## Acceptance result and limitations

| Item | Result |
|---|---|
| Worktree clean before execution | Verified. |
| Full current pytest | 158 passed, 2 skipped, exit 0. |
| Global static review | PASS / APPROVED / Ready YES; 0 Critical, 0 Important. |
| Physical Unity | 38/38 passed on COM3 via documented serial fallback. |
| Portrait build | Passed. |
| Landscape final rebuild | Passed and used for flash. |
| Conservative flash | 4/4 hashes verified plus hard reset. |
| Serial production restore | COM3 identified; 2 sends; 0 ACK/write failures; both tray PIDs alive. |
| Exactly three pages / no Robot dashboard | Hardware Unity and static/contract coverage passed; direct screen observation unavailable. |
| Claude mark / Blossom / both on Activity | Hardware Unity branding mask and layout contracts passed; direct screen observation unavailable. |
| Semantic colors and unavailable states | Hardware Unity/parser contracts passed; direct color/text observation unavailable. |
| Left previous / right next | Hardware Unity midpoint and carousel cases passed; no manual touch was performed. |
| Upright and unmirrored landscape | Landscape build/transform Unity passed and final firmware is running; no camera/operator evidence was available. |

The tray and definitive landscape firmware were deliberately left running. The
branch and worktree were preserved. No push was performed.
