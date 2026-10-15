#!/usr/bin/env python3
"""Emit ESP Web Tools manifests (the browser-flasher fallback) from manifest.json.

    python3 tools/ci/make_webtools_manifest.py --dist dist [--base-url URL]

ESP Web Tools picks a build by chip family only, so several boards sharing one
chip (four S3 boards, two C6 boards) cannot live in one manifest. This writes
one manifest per board, dist/webtools-<env>.json, each with a single build.

The merged image covers the NVS partition (BLE bonds, settings) with 0xFF
padding, and an update that wrote it would unpair the board. So the image is
split around NVS into <env>-<version>.part<N>.bin files, one part each; an
image with no partition table ships whole at offset 0:

    {"name": "Clawdmeter — <display name>", "version": "v1.9.0",
     "new_install_prompt_erase": true,
     "builds": [{"chipFamily": "ESP32-S3",
                 "parts": [{"path": "<env>-v1.9.0.part0.bin", "offset": 0},
                           {"path": "<env>-v1.9.0.part1.bin", "offset": 57344}]}]}

`path` is resolved relative to the manifest URL; pass --base-url to make it
absolute (e.g. when the manifests are served from a Pages site and the images
from the Release).
"""

import argparse
import json
import struct
import sys
from pathlib import Path

PARTITION_TABLE = 0x8000


def preserved_ranges(image: bytes) -> list[tuple[int, int]]:
    """NVS and NVS-keys partitions, as (start, end), from the image's own table.

    Mirrors clawdmeter-flash's device::preserved_ranges."""
    out = []
    table = image[PARTITION_TABLE:PARTITION_TABLE + 0xC00]
    for i in range(0, len(table) - 31, 32):
        e = table[i:i + 32]
        if e[:2] != b"\xaa\x50":
            break
        kind, subtype = e[2], e[3]
        off, size = struct.unpack("<II", e[4:12])
        if kind == 0x01 and subtype in (0x02, 0x04):
            out.append((off, off + size))
    return sorted(out)


def write_segments(length: int, keep: list[tuple[int, int]]) -> list[tuple[int, int]]:
    """The parts of 0..length outside `keep`."""
    out, at = [], 0
    for start, end in keep:
        if start >= length:
            break
        if start > at:
            out.append((at, start))
        at = max(at, end)
    if at < length:
        out.append((at, length))
    return out


def split_image(dist: Path, file: str, offset: int) -> list[dict]:
    """Write the image's parts next to it; return them as (path, offset) parts."""
    image = (dist / file).read_bytes()
    segments = write_segments(len(image), preserved_ranges(image))
    if segments == [(0, len(image))]:
        return [{"path": file, "offset": offset}]
    stem = file[:-4] if file.endswith(".bin") else file
    parts = []
    for n, (start, end) in enumerate(segments):
        name = f"{stem}.part{n}.bin"
        (dist / name).write_bytes(image[start:end])
        parts.append({"path": name, "offset": offset + start})
    return parts

# esptool chip name -> ESP Web Tools chipFamily
CHIP_FAMILIES = {
    "esp32": "ESP32",
    "esp32s2": "ESP32-S2",
    "esp32s3": "ESP32-S3",
    "esp32c3": "ESP32-C3",
    "esp32c6": "ESP32-C6",
    "esp32h2": "ESP32-H2",
}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--dist", type=Path, required=True)
    ap.add_argument("--manifest", type=Path, help="default: <dist>/manifest.json")
    ap.add_argument("--base-url", default="", help="prefix for part paths")
    args = ap.parse_args()

    manifest = json.loads((args.manifest or args.dist / "manifest.json").read_text())
    base = args.base_url.rstrip("/") + "/" if args.base_url else ""
    for board in manifest["boards"]:
        family = CHIP_FAMILIES.get(board["chip"])
        if family is None:
            print(f"error: no ESP Web Tools family for chip {board['chip']}", file=sys.stderr)
            return 1
        web = {
            "name": f"Clawdmeter — {board['display_name']}",
            "version": manifest["version"],
            "new_install_prompt_erase": True,
            "builds": [{
                "chipFamily": family,
                "parts": [{"path": base + p["path"], "offset": p["offset"]}
                          for p in split_image(args.dist, board["file"], board["offset"])],
            }],
        }
        out = args.dist / f"webtools-{board['env']}.json"
        out.write_text(json.dumps(web, indent=2, ensure_ascii=False) + "\n")
        print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
