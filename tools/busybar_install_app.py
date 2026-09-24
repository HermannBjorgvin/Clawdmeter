#!/usr/bin/env python3
"""Install the Petmeter app onto a BUSY Bar, so it appears in the apps menu.

    python3 tools/busybar_install_app.py [http://10.0.4.20] [--remove]

There is no app-installation API. Apps are directories under
`/ext/user_assets/`, each holding `appmeta/manifest.json`, an 8x8 front icon,
an 11x11 back icon and `scripts/main.js` — the shape the device's own
`app.busy.js_example` uses. This creates that layout with the storage API.

The JS SDK is documented as "coming soon", so the on-device runtime
(`apps_assets/js_runner`) and this layout are undocumented and may change
under a firmware update. If the app stops appearing after one, compare
against `app.busy.js_example` again, which ships with the firmware and will
have moved with it.
"""
import sys
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

APP_ID = "app.petmeter"
SRC = Path(__file__).resolve().parent.parent / "busybar" / APP_ID
REMOTE_ROOT = f"/ext/user_assets/{APP_ID}"
TIMEOUT = 30


def _call(base: str, path: str, params: dict, data: bytes | None = None,
          method: str = "GET") -> tuple[int, str]:
    url = f"{base.rstrip('/')}{path}?{urllib.parse.urlencode(params)}"
    req = urllib.request.Request(url, data=data, method=method)
    if data is not None:
        req.add_header("Content-Type", "application/octet-stream")
    try:
        with urllib.request.urlopen(req, timeout=TIMEOUT) as resp:
            return resp.status, resp.read().decode(errors="replace")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode(errors="replace")


def install(base: str) -> int:
    if not SRC.is_dir():
        print(f"missing source tree: {SRC}")
        return 1

    for d in (REMOTE_ROOT, f"{REMOTE_ROOT}/appmeta", f"{REMOTE_ROOT}/scripts"):
        code, body = _call(base, "/api/storage/mkdir", {"path": d}, method="POST")
        # An existing directory is a 400 here, which is success for our purpose.
        print(f"  mkdir {d}: {code}" + ("" if code == 200 else f" {body.strip()}"))

    failures = 0
    for local in sorted(SRC.rglob("*")):
        if local.is_dir():
            continue
        remote = f"{REMOTE_ROOT}/{local.relative_to(SRC).as_posix()}"
        code, body = _call(base, "/api/storage/write", {"path": remote},
                           data=local.read_bytes(), method="POST")
        print(f"  write {remote} ({local.stat().st_size}B): {code}"
              + ("" if code == 200 else f" {body.strip()}"))
        failures += code != 200

    if failures:
        print(f"\n{failures} file(s) failed.")
        return 1
    print("\nInstalled. Restart the bar (or reopen the apps menu) and pick "
          "'Petmeter'.\nIt pulls from the daemon, so set `busybar_serve = "
          "10.0.4.21:8724` in\n~/.config/claude-usage-monitor/config and "
          "restart the daemon.")
    return 0


def remove(base: str) -> int:
    code, body = _call(base, "/api/storage/remove", {"path": REMOTE_ROOT},
                       method="DELETE")
    print(f"remove {REMOTE_ROOT}: {code} {body.strip()}")
    return 0 if code == 200 else 1


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    url = args[0] if args else "http://10.0.4.20"
    sys.exit(remove(url) if "--remove" in sys.argv else install(url))
