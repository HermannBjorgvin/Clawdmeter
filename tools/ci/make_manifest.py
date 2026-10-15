#!/usr/bin/env python3
"""Build the release manifest.json from merge_firmware.py's per-env sidecars.

    python3 tools/ci/make_manifest.py --version v1.9.0 --dist dist

Reads dist/<env>-<version>.json for every env, verifies each image's sha256
against the file on disk, and writes dist/manifest.json:

    {
      "schema": 1,
      "version": "v1.9.0",
      "boards": [
        {"env": "...", "chip": "esp32s3", "flash_size": "16MB",
         "display_name": "...", "file": "<env>-v1.9.0.bin",
         "offset": 0, "size": 2235664, "sha256": "..."}
      ]
    }

The desktop flasher (desktop/crates/clawdmeter-flash) reads this file from the
latest GitHub Release; `file` is the release asset name, always written at
`offset` 0. Adding a board: give it a DISPLAY_NAMES entry below. An env with no
entry still ships (named after the env) but the run prints a warning.
"""

import argparse
import hashlib
import json
import sys
from pathlib import Path

SCHEMA = 1

# env -> human name shown in the flasher's board picker. Keep in sync with the
# [env:...] blocks in firmware/platformio.ini.
DISPLAY_NAMES = {
    "waveshare_amoled_216": "Waveshare ESP32-S3 AMOLED 2.16\"",
    "waveshare_amoled_18": "Waveshare ESP32-S3 AMOLED 1.8\"",
    "waveshare_amoled_216_c6": "Waveshare ESP32-C6 AMOLED 2.16\"",
    "waveshare_amoled_18_c6": "Waveshare ESP32-C6 AMOLED 1.8\"",
    "waveshare_amoled_206": "Waveshare ESP32-S3 AMOLED 2.06\" (watch)",
    "waveshare_lcd_154": "Waveshare ESP32-S3 LCD 1.54\"",
    "waveshare_lcd_4": "Waveshare ESP32-S3 LCD 4\"",
}

REQUIRED = ("env", "chip", "flash_size", "file", "sha256")


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def load_boards(dist: Path, version: str) -> list[dict]:
    boards = []
    for sidecar in sorted(dist.glob(f"*-{version}.json")):
        info = json.loads(sidecar.read_text())
        missing = [k for k in REQUIRED if k not in info]
        if missing:
            raise ValueError(f"{sidecar.name}: missing {missing}")
        image = dist / info["file"]
        if not image.is_file():
            raise ValueError(f"{sidecar.name}: image {info['file']} not in {dist}")
        digest = sha256(image)
        if digest != info["sha256"]:
            raise ValueError(f"{info['file']}: sha256 {digest} != sidecar {info['sha256']}")
        name = DISPLAY_NAMES.get(info["env"])
        if name is None:
            print(f"warning: no DISPLAY_NAMES entry for {info['env']}", file=sys.stderr)
            name = info["env"]
        boards.append({
            "env": info["env"],
            "chip": info["chip"],
            "flash_size": info["flash_size"],
            "display_name": name,
            "file": info["file"],
            "offset": 0,
            "size": image.stat().st_size,
            "sha256": digest,
        })
    return boards


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--version", required=True)
    ap.add_argument("--dist", type=Path, required=True)
    ap.add_argument("--out", type=Path, help="default: <dist>/manifest.json")
    ap.add_argument("--expect", help="JSON array of envs that must all be present "
                    "(e.g. the output of hardware_envs.py)")
    args = ap.parse_args()

    try:
        boards = load_boards(args.dist, args.version)
    except ValueError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    if not boards:
        print(f"error: no *-{args.version}.json sidecars in {args.dist}", file=sys.stderr)
        return 1
    if args.expect:
        want = set(json.loads(args.expect))
        got = {b["env"] for b in boards}
        if want - got:
            print(f"error: missing images for {sorted(want - got)}", file=sys.stderr)
            return 1

    manifest = {"schema": SCHEMA, "version": args.version, "boards": boards}
    out = args.out or args.dist / "manifest.json"
    out.write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"wrote {out} ({len(boards)} boards)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
