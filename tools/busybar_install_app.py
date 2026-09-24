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
import socket
import sys
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

APP_ID = "app.petmeter"
# What `busy-cli build` produced. Built, not hand-assembled: the official
# toolchain compiles src/main.ts, bundles appmeta/ and the mascot assets, and
# names the folder after the app id.
SRC = (Path(__file__).resolve().parent.parent
       / "busybar" / "petmeter" / "dist" / APP_ID)
REMOTE_ROOT = f"/ext/user_assets/{APP_ID}"
TIMEOUT = 30
# One write call has a ceiling somewhere under 36 KB -- a bundled main.js came
# back 508 "Failed to open file for writing" while a 4 KB one went up fine. The
# API takes an `append` flag, so anything larger goes up in pieces.
CHUNK = 8192
CLI_PORT = 23          # the device's telnet CLI, used only to stop a running app


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


def stop_running(host: str) -> None:
    """Stop any running JS app before writing over it.

    A running app holds its script open, so replacing it under a live process
    is not something to rely on. The device's CLI is a plain telnet server on
    port 23.
    """
    try:
        with socket.create_connection((host, CLI_PORT), timeout=8) as cli:
            cli.settimeout(3)
            try:
                cli.recv(4096)                      # banner
                cli.sendall(b"js -k\r\n")
                cli.recv(4096)
            except socket.timeout:
                pass
        print("  stopped any running app")
    except OSError as exc:
        # Not fatal: nothing may be running, or the CLI may be off.
        print(f"  (could not reach the CLI to stop a running app: {exc})")


def install(base: str) -> int:
    if not SRC.is_dir():
        print(f"missing source tree: {SRC}")
        return 1

    stop_running(urllib.parse.urlparse(base).hostname or base)

    for d in (REMOTE_ROOT, f"{REMOTE_ROOT}/appmeta", f"{REMOTE_ROOT}/scripts"):
        code, body = _call(base, "/api/storage/mkdir", {"path": d}, method="POST")
        # An existing directory is a 400 here, which is success for our purpose.
        print(f"  mkdir {d}: {code}" + ("" if code == 200 else f" {body.strip()}"))

    failures = 0
    for local in sorted(SRC.rglob("*")):
        if local.is_dir():
            continue
        remote = f"{REMOTE_ROOT}/{local.relative_to(SRC).as_posix()}"
        blob = local.read_bytes()
        # The device will not write over an existing file -- it answers 508
        # "Failed to open file for writing", which reads like a disk fault and
        # is really "this path is taken". Clear it first; a missing file is a
        # fine outcome too, so the result is ignored.
        _call(base, "/api/storage/remove", {"path": remote}, method="DELETE")
        code, body, parts = 200, "", 0
        for offset in range(0, max(len(blob), 1), CHUNK):
            params = {"path": remote}
            if offset:
                params["append"] = 1
            code, body = _call(base, "/api/storage/write", params,
                               data=blob[offset:offset + CHUNK], method="POST")
            parts += 1
            if code != 200:
                break
        suffix = f" in {parts} chunks" if parts > 1 else ""
        print(f"  write {remote} ({len(blob)}B){suffix}: {code}"
              + ("" if code == 200 else f" {body.strip()}"))
        failures += code != 200

    if failures:
        print(f"\n{failures} file(s) failed.")
        return 1
    print("\nInstalled. The apps menu is a hardcoded list in this firmware and "
          "will not\nshow it (see docs/busybar.md), so launch it over the "
          "device's telnet CLI:\n\n"
          f"  js -i {APP_ID} {REMOTE_ROOT}/scripts/main.js\n\n"
          "It pulls from the daemon, so set `busybar_serve = 10.0.4.21:8724` "
          "in\n~/.config/claude-usage-monitor/config and restart the daemon.")
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
