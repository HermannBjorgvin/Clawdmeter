#!/usr/bin/env python3
"""Print the PlatformIO hardware envs as a JSON array, for CI build matrices.

Reads firmware/platformio.ini directly (no PlatformIO install needed) and
keeps every [env:*] whose platform is not `native`, so the desktop simulator
and any host-side test envs never end up in the firmware matrix. Adding a
board is a new [env:...] block; CI picks it up without a workflow edit.

    python3 tools/ci/hardware_envs.py [--ini firmware/platformio.ini]
    python3 tools/ci/hardware_envs.py --native   # host envs instead
    python3 tools/ci/hardware_envs.py --matrix   # [{"env": ..., "board": ...}]

--matrix emits objects for a `matrix.include`, so a job can key its
toolchain cache on the board (all S3 envs share one cache, all C6 envs another).
"""

import argparse
import configparser
import json
import sys
from pathlib import Path

DEFAULT_INI = Path(__file__).resolve().parents[2] / "firmware" / "platformio.ini"


def read_envs(ini_path: Path) -> list[tuple[str, dict]]:
    cp = configparser.ConfigParser(
        inline_comment_prefixes=(";", "#"),
        comment_prefixes=(";", "#"),
        interpolation=None,
        strict=False,
    )
    cp.read(ini_path, encoding="utf-8")
    envs = []
    for section in cp.sections():
        if section.startswith("env:"):
            envs.append((section[len("env:"):], dict(cp[section])))
    return envs


def is_native(opts: dict) -> bool:
    return opts.get("platform", "").strip() == "native"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--ini", type=Path, default=DEFAULT_INI)
    ap.add_argument("--native", action="store_true",
                    help="list native (host) envs instead of hardware envs")
    ap.add_argument("--matrix", action="store_true",
                    help="emit [{env, board}] objects instead of bare names")
    args = ap.parse_args()

    envs = read_envs(args.ini)
    picked = [(n, o) for n, o in envs if is_native(o) == args.native]
    if not picked:
        print(f"no matching envs in {args.ini}", file=sys.stderr)
        return 1
    if args.matrix:
        print(json.dumps([{"env": n, "board": o.get("board", "native").strip()}
                          for n, o in picked]))
    else:
        print(json.dumps([n for n, _ in picked]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
