# Development

Run commands from the repository root unless shown otherwise.

## Build from source

Firmware uses PlatformIO. Select the environment for your board:

```bash
pio run -d firmware -e waveshare_amoled_216
pio run -d firmware -e waveshare_amoled_216 -t upload --upload-port /dev/ttyACM0
pio test -d firmware -e native
```

Environments are listed in `firmware/platformio.ini`. macOS ports use
`/dev/cu.usbmodem*`; Windows ports use `COMn`. A bare `pio run -d firmware`
builds the default hardware environments and simulator.

If a build fails on a framework header that exists, such as
`SD.cpp: fatal error: ff.h`, an env with `lib_ignore` has left the shared
framework package edited (a metadata scan, which the VS Code extension runs
whenever it loads such an env, applies the edit without undoing it). Run
`python3 tools/ci/restore_pio_build_scripts.py` before building the failing
env; the next scan of a `lib_ignore` env edits it again.

The desktop app uses Rust 1.90 or newer. Install the Linux packages listed
in [the build dependency action](../.github/actions/linux-desktop-deps/action.yml), then:

```bash
cd desktop
cargo run -p clawdmeter-app --release
cargo test --workspace
```

The workspace separates usage/configuration (`clawdmeter-core`), Bluetooth
(`clawdmeter-ble`), firmware downloads and flashing (`clawdmeter-flash`), and
the tray, GPUI window, and hook integration (`clawdmeter-app`).

## Simulator and releases

Use the [firmware simulator](simulator.md) to review layouts and Clawd's
behavior without hardware. See [font generation](fonts.md) for font builds.
Desktop reference images are in
[desktop/docs/screenshots](../desktop/docs/screenshots/).

The [release workflow](../.github/workflows/release.yml) accepts tags in the
form `vMAJOR.MINOR.PATCH`, with an optional prerelease suffix. It builds
merged board images for address `0x0`, desktop archives, checksums, a
`manifest.json` for the app, and ESP Web Tools manifests. Tags with a
hyphen publish prereleases, which the normal update check ignores.

See the [porting guide](porting/adding-a-board.md) for the full checklist.
A new board needs a display-name entry in `tools/ci/make_manifest.py` as
well as its firmware environment and desktop board assets. Firmware CI
builds the boards, runs native tests, and captures the simulator. Desktop
CI runs formatting, linting, and workspace tests on Linux, plus core tests
on macOS and Windows.
