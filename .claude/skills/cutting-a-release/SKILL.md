---
name: cutting-a-release
description: Prepares and publishes a Clawdmeter release - version tags (vX.Y.Z, -rc prereleases), what release.yml builds (merged firmware images, manifest.json, ESP Web Tools manifests, desktop archives, dmg/msi/deb/AppImage, SHA256SUMS), and the local packaging and icon checks to run first. Use when asked to cut, tag or publish a release or release candidate, check a release is ready, or debug the release workflow or packaging.
---

# Cutting a release

Pushing a tag publishes a GitHub Release that users' apps see. Never push a
tag without the owner's explicit go-ahead for that exact tag.

## Tags

`vMAJOR.MINOR.PATCH`, with an optional prerelease suffix (`v2.1.0-rc.1`). The
workflow rejects anything else. Tags with a hyphen publish as prereleases,
which the desktop app's update check skips. See existing tags with
`git tag --sort=-creatordate | head`.

## Before tagging

```
Release checks:
- [ ] main is green: firmware.yml and desktop.yml passed on the commit
- [ ] Every hardware env has a DISPLAY_NAMES entry in tools/ci/make_manifest.py
- [ ] python3 tools/packaging/make_icons.py --check   (icons match the firmware art)
- [ ] python3 -m unittest discover -s tools/ci/tests
- [ ] Packaging changed? Run the local checks below
- [ ] .github/ changed? actionlint (+ shellcheck) passes
```

Local packaging checks (Linux), after `cargo build -p clawdmeter-app --release`
in `desktop/`:

```bash
(cd desktop && cargo deb -p clawdmeter-app --no-build)        # needs cargo-deb
tools/packaging/appimage.sh desktop/target/release/clawdmeter /tmp/clawdmeter.AppImage /tmp/appimage-work
```

## What release.yml builds

The header comment of `.github/workflows/release.yml` lists the assets; in
short:

- `<env>-<tag>.bin`: one merged image per hardware env, flashed at `0x0`,
  built with `CLAWD_VERSION` set to the tag.
- `manifest.json` (`tools/ci/make_manifest.py`): version plus per-board
  chip, flash size, display name, file, offset, size, sha256. The app's
  flasher matches boards by chip and flash size from it.
- `webtools-<env>.json` (`tools/ci/make_webtools_manifest.py`): ESP Web Tools.
- Desktop archives built with `CLAWDMETER_VERSION=${tag#v}`: Linux binary,
  macOS universal `Clawdmeter.app` (ad-hoc signed, `NSBluetoothAlwaysUsageDescription`
  + `LSUIElement`), Windows exe.
- Installers: `.dmg` (hdiutil), per-user `.msi` from
  `tools/packaging/wix/clawdmeter.wxs` (WiX 5), `.deb` from
  `[package.metadata.deb]` in `desktop/crates/clawdmeter-app/Cargo.toml`,
  `.AppImage` from `tools/packaging/appimage.sh` on ubuntu-22.04 (host
  Vulkan/Wayland/D-Bus, nothing GPU-side bundled).
- `SHA256SUMS`.

Nothing is signed beyond the ad-hoc macOS signature. MSI versions are
numeric, so `v1.2.0-rc.1` installs as `1.2.0`.

Packaging assets live in `tools/packaging/`: icons (PNG/ICO/ICNS rendered
from the official idle Clawd by `make_icons.py`, committed; the release fails
if they drift from the firmware art) and `linux/clawdmeter.desktop`
(app_id `clawdmeter`).

## Nix

`flake.nix` (Linux only) builds the app with `nix build .#clawdmeter`
(Vulkan/Wayland added to the rpath rather than wrapped, because the app
writes its own path into hooks and autostart). Bump inputs with
`nix flake update`.
