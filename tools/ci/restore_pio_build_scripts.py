#!/usr/bin/env python3
"""Undo the pioarduino platform's in-place edits to the shared framework package.

For an env with `lib_ignore`, the pinned pioarduino platform rewrites
`framework-arduinoespressif32-libs/<mcu>/pioarduino-build.py` in
~/.platformio/packages to drop the ignored libraries' include paths (SD drops
`fatfs`, where `ff.h` lives). It first copies the original to
`pioarduino-build.py.<mcu>` and copies it back after `checkprogsize`, the end
of a full `pio run`. Anything that runs the build scripts without that target,
such as `pio project metadata` in merge_firmware.py, leaves the edit in place.
CI then caches the edited package, and the next env that needs the dropped
headers fails to compile with nothing in its own config to explain it.

This puts every such backup back, the same way the platform does. CI runs it
after each package-cache restore (.github/actions/setup-pio). It covers the
build-script edits only: `custom_component_remove` would delete include
directories from the package with no backup, and nothing here uses it.

Locally, the VS Code PlatformIO extension runs the same metadata scan whenever
it loads an env with lib_ignore, so run this before building an env that needs
the dropped headers:

    python3 tools/ci/restore_pio_build_scripts.py
"""

import os
import re
import shutil
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
BUILD_SCRIPT = "pioarduino-build.py"
# The platform's backup name: the build script plus the chip, e.g. `.esp32s3`.
BACKUP = re.compile(re.escape(BUILD_SCRIPT) + r"\.esp32[a-z0-9]*")


def packages_dir() -> Path:
    """PlatformIO's packages directory, resolved by PlatformIO when it is
    importable (env vars, the project's [platformio] options, the Windows
    fallback), else from PLATFORMIO_CORE_DIR or ~/.platformio."""
    try:
        from platformio.project.config import ProjectConfig
        ini = REPO / "firmware" / "platformio.ini"
        return Path(ProjectConfig.get_instance(str(ini)).get("platformio", "packages_dir"))
    except Exception:
        core = os.environ.get("PLATFORMIO_CORE_DIR")
        return (Path(core) if core else Path.home() / ".platformio") / "packages"


def restore(packages: Path) -> list[Path]:
    """Copy each `pioarduino-build.py.<mcu>` backup over its build script and
    delete the backup. Returns the build scripts that were restored."""
    restored = []
    # `-libs*` also matches a second installed version (`-libs@<version>`).
    for backup in sorted(packages.glob(f"framework-arduinoespressif32-libs*/*/{BUILD_SCRIPT}.*")):
        if not BACKUP.fullmatch(backup.name):
            continue
        target = backup.with_name(BUILD_SCRIPT)
        shutil.copy2(backup, target)
        backup.unlink()
        restored.append(target)
    return restored


def main() -> int:
    packages = packages_dir()
    restored = restore(packages)
    for path in restored:
        print(f"restored {path} from the platform's lib_ignore backup")
    if not restored:
        print(f"no lib_ignore edits to undo under {packages}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
