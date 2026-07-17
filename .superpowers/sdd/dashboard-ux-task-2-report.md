# Dashboard UX Task 2 Report

## Status

Implemented page-aware Claude/OpenAI branding and added the official OpenAI
Blossom as a local LVGL RGB565A8 asset. No payload, daemon, COM3, tray, or
Robot-page implementation was changed.

## Official asset provenance

- Brand guidelines: `https://openai.com/brand/`
- Official download link exposed by the page after accepting the Marks terms:
  `https://cdn.openai.com/brand/openai-logos.zip`
- ZIP SHA-256:
  `c54e85ab5884228f89f0230dd8effa8d588cad78166fe954135f4afa553222db`
- Selected file inside the ZIP:
  `OpenAI-logos/PNGs/OAI_OpenAI-Blossom_White.png`
- Official source dimensions/mode: `716x716`, RGBA
- Official source alpha bounds: `(180, 180, 536, 536)`; the clear space supplied
  by OpenAI was retained by resizing the entire square canvas.
- Official source SHA-256:
  `21c7057cfbec1b3892b7c3d724c57b914755ca66e4178f331072782cbc86525b`
- Local output: `assets/openai_blossom_80.png`, `80x80`, RGBA, white on
  transparency, alpha bounds `(18, 18, 62, 62)`.
- Local PNG SHA-256:
  `fab8e40f1bb50814dec3c9b590bd72635873bc94da9ef7d054ffe400e6a759f4`
- RGB565A8 header: `firmware/src/codex_logo.h`, exactly 19,200 bytes encoded
  as 12,800 RGB565 bytes followed by 6,400 alpha bytes.
- Header SHA-256:
  `f8465e56327558d11f9e4d58884a0e3beb347b304db6b5643558b9b3c8dfba89`

The asset was not redrawn, recolored, cropped, or distorted. Pillow resized
the official white PNG from 716x716 to 80x80 using LANCZOS, then the conversion
snippet from the task brief generated the RGB565A8 array mechanically.

## RED

Tests were written before production code:

```powershell
& 'C:\Users\Gustavo\Documents\esp32-claude\.venv\Scripts\python.exe' -m pytest tests\test_esp32_2432s024c_contract.py -q
& 'C:\Users\Gustavo\.platformio\penv\Scripts\pio.exe' test -d firmware -e esp32_2432s024c -f test_port_helpers --without-uploading --without-testing
```

Observed RED:

- Pytest: `3 failed, 17 passed`; failures were the missing provider image
  objects, missing `codex_logo.h`, and missing 19,200-byte Codex asset.
- Unity compile/link: exit `1`; compilation stopped because
  `dashboard_branding.h` did not exist.

## Minimal implementation

- Added `DashboardBrandMask` and `dashboard_brand_mask(DashboardPage)`.
- Preserved the existing Claude `logo_data` and created separate Claude and
  Codex LVGL descriptors/images with top-left pivots and the existing scale.
- Claude is shown on Claude; Codex on Codex; both on Activity; neither on Robot.
- On Activity, Codex moves to the right edge while Claude stays at the left.
- Both logo images move to the foreground immediately before the transparent
  navigation layer.

## GREEN and builds

Focused verification:

- Pytest: `20 passed in 0.06s`, exit `0` on the final rerun.
- Unity compile/link: `PASSED`, exit `0`, duration `65.31s` on the final rerun.
- The linker emitted its pre-existing toolchain warning about
  `_fixdfdi.o` lacking `.note.GNU-stack`; it did not fail the link.

Build commands:

```powershell
& 'C:\Users\Gustavo\.platformio\penv\Scripts\pio.exe' run -d firmware -e esp32_2432s024c -j 1
& 'C:\Users\Gustavo\.platformio\penv\Scripts\pio.exe' run -d firmware -e esp32_2432s024c_landscape -j 1
```

- Portrait: `SUCCESS`, exit `0`, duration `31.075s`; RAM 32.3%, flash 42.4%.
- Landscape: `SUCCESS`, exit `0`, duration `32.789s`; RAM 32.3%, flash
  42.4%.

## Bounds and self-review

- Portrait Activity Codex frame: x `188..230` within `240`, y `6..48`
  within `320`.
- Landscape Activity Codex frame: x `270..310` within `320`, y `6..46`
  within `240`.
- `git diff --check` passed; only expected CRLF conversion notices appeared.
- Visual inspection confirmed a centered, undistorted monochrome Blossom with
  transparent clear space.
- Search confirmed the old shared `logo_img`/`logo_dsc` identifiers are gone.
- No push, upload, COM3 access, tray changes, or payload/daemon changes were
  performed.
