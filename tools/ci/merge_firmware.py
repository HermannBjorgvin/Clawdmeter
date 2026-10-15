#!/usr/bin/env python3
"""Merge one built PlatformIO env into a single image flashed at offset 0x0.

Run with the Python that has PlatformIO installed (its API resolves the board
config) and after `pio run -e <env>` has finished, in the same environment
(same PLATFORMIO_BUILD_FLAGS): `pio project metadata` re-runs the build graph,
and a flag mismatch would make PlatformIO clean the build directory.

    python tools/ci/merge_firmware.py --env waveshare_amoled_216 \
        --version v1.9.0 --out-dir dist

Produces, in --out-dir:
    <env>-<version>.bin    bootloader + partition table + boot_app0 + app
    <env>-<version>.json   sidecar {env, chip, flash_size, file, sha256, ...}
                           consumed by make_manifest.py

Where each input comes from:
  * images + offsets   `pio project metadata` (extra.flash_images), the same
                       list `pio run -t upload` writes
  * app offset         the first app partition in the built partitions.bin
  * chip               the chip id in bootloader.bin's extended header
  * flash mode/freq/size  the board config (with board_* overrides from
                       platformio.ini), using the same rules as the
                       pioarduino builder's own firmware.factory.bin step

If PlatformIO also produced firmware.factory.bin, the merged image must match
it byte for byte; a mismatch fails the run, since it means the rules above
have drifted from the platform's.
"""

import argparse
import hashlib
import json
import os
import struct
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

# esptool's chip ids (esp_image_header_t.chip_id) -> esptool --chip names
CHIP_IDS = {
    0x0000: "esp32",
    0x0002: "esp32s2",
    0x0005: "esp32c3",
    0x0009: "esp32s3",
    0x000C: "esp32c2",
    0x000D: "esp32c6",
    0x0010: "esp32h2",
    0x0012: "esp32p4",
    0x0017: "esp32c5",
}


def run_json(cmd: list[str], cwd: Path) -> dict:
    out = subprocess.run(cmd, cwd=cwd, check=True, capture_output=True, text=True)
    # pio may print notices before the JSON; take the first line that parses
    for line in out.stdout.splitlines():
        line = line.strip()
        if line.startswith("{"):
            return json.loads(line)
    raise RuntimeError(f"no JSON in output of {' '.join(cmd)}:\n{out.stdout}")


def chip_from_bootloader(path: Path) -> str:
    data = path.read_bytes()[:24]
    if len(data) < 24 or data[0] != 0xE9:
        raise ValueError(f"{path} is not an ESP image (magic {data[:1].hex()})")
    # 8-byte common header, then extended header: wp_pin(1) spi_drv(3) chip_id(2)
    (chip_id,) = struct.unpack_from("<H", data, 12)
    if chip_id not in CHIP_IDS:
        raise ValueError(f"unknown chip id 0x{chip_id:04x} in {path}")
    return CHIP_IDS[chip_id]


def app_offset_from_partitions(path: Path) -> int:
    data = path.read_bytes()
    for i in range(0, len(data) - 31, 32):
        magic, ptype, _subtype, offset, _size = struct.unpack_from("<HBBII", data, i)
        if magic == 0xEBEB:  # MD5 checksum entry
            continue
        if magic != 0x50AA:  # end of table
            break
        if ptype == 0x00:  # first app partition (factory or ota_0) is the boot target
            return offset
    raise ValueError(f"no app partition in {path}")


def normalize_freq(freq: str) -> str:
    return f"{int(str(freq).replace('L', '')) // 1_000_000}m"


def board_flash_params(project_dir: Path, env: str) -> dict:
    """Flash mode/freq/size exactly as pioarduino's esp32_create_combined_bin."""
    from platformio.platform.factory import PlatformFactory
    from platformio.project.config import ProjectConfig

    os.chdir(project_dir)
    cfg = ProjectConfig.get_instance(str(project_dir / "platformio.ini"))
    section = f"env:{env}"
    platform = PlatformFactory.from_env(env, autoinstall=False)
    board = platform.board_config(cfg.get(section, "board"))
    # board_build.x / board_upload.x in platformio.ini override the board JSON
    for opt in cfg.options(section):
        if opt.startswith("board_") and "." in opt:
            board.update(opt[len("board_"):], cfg.get(section, opt))

    framework = "_".join(cfg.get(section, "framework", []) or ["arduino"])
    default_mem = f"{board.get('build.flash_mode', 'dio')}_{board.get('build.psram_type', 'qspi')}"
    memory_type = board.get("build.memory_type",
                            board.get(f"build.{framework}.memory_type", default_mem))

    mode = board.get("build.flash_mode", "dio")
    if memory_type in ("opi_opi", "opi_qspi"):
        mode = "dout"
    elif mode in ("qio", "qout"):
        mode = "dio"

    freq = board.get("build.f_image", board.get("build.f_flash", "40000000L"))
    return {
        "mcu": board.get("build.mcu"),
        "flash_mode": mode,
        "flash_freq": normalize_freq(freq),
        "flash_size": board.get("upload.flash_size", "4MB"),
    }


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--env", required=True)
    ap.add_argument("--version", required=True, help="release tag, e.g. v1.9.0")
    ap.add_argument("--project-dir", type=Path, default=REPO / "firmware")
    ap.add_argument("--out-dir", type=Path, default=REPO / "dist")
    ap.add_argument("--pio", default=os.environ.get("PIO", "pio"),
                    help="pio executable (default: $PIO or 'pio')")
    args = ap.parse_args()

    project_dir = args.project_dir.resolve()
    out_dir = args.out_dir.resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    build_dir = project_dir / ".pio" / "build" / args.env

    meta = run_json([args.pio, "project", "metadata", "-d", str(project_dir),
                     "-e", args.env, "--json-output"], cwd=project_dir)[args.env]
    images = [(int(i["offset"], 16), Path(i["path"])) for i in meta["extra"]["flash_images"]]
    app = build_dir / "firmware.bin"
    partitions = next((p for _, p in images if p.name == "partitions.bin"),
                      build_dir / "partitions.bin")
    bootloader = next((p for o, p in images if o == 0), build_dir / "bootloader.bin")
    for p in [app, partitions, bootloader, *(p for _, p in images)]:
        if not p.is_file():
            print(f"missing build output: {p} (run `pio run -e {args.env}` first)",
                  file=sys.stderr)
            return 1

    app_offset = app_offset_from_partitions(partitions)
    chip = chip_from_bootloader(bootloader)
    params = board_flash_params(project_dir, args.env)
    if params["mcu"] and params["mcu"] != chip:
        print(f"board mcu {params['mcu']} != bootloader chip {chip}", file=sys.stderr)
        return 1

    name = f"{args.env}-{args.version}.bin"
    out = out_dir / name
    parts: list[str] = []
    for offset, path in sorted([*images, (app_offset, app)]):
        parts += [hex(offset), str(path)]
    cmd = [sys.executable, "-m", "esptool", "--chip", chip, "merge-bin",
           "-o", str(out),
           "--flash-mode", params["flash_mode"],
           "--flash-freq", params["flash_freq"],
           "--flash-size", params["flash_size"], *parts]
    print("+", " ".join(cmd), flush=True)
    subprocess.run(cmd, check=True)

    factory = build_dir / "firmware.factory.bin"
    if factory.is_file() and factory.read_bytes() != out.read_bytes():
        print(f"{out.name} differs from PlatformIO's {factory}; the flash-parameter "
              "rules in this script no longer match the platform's", file=sys.stderr)
        return 1

    info = {
        "env": args.env,
        "chip": chip,
        "flash_size": params["flash_size"],
        "flash_mode": params["flash_mode"],
        "flash_freq": params["flash_freq"],
        "app_offset": hex(app_offset),
        "file": name,
        "size": out.stat().st_size,
        "sha256": sha256(out),
    }
    (out_dir / f"{args.env}-{args.version}.json").write_text(json.dumps(info, indent=2) + "\n")
    print(json.dumps(info, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
