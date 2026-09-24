#!/usr/bin/env python3
"""Install the Petmeter app onto a BUSY Bar, so it appears in the apps menu.

    python3 tools/busybar_install_app.py [http://10.0.4.20] [--remove] [--enable-menu]

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
import http.client
import socket
import sys
import time
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
# The storage API drops a call now and then. Over USB, three runs in six died
# with `TimeoutError: timed out` partway through the upload, each time on the
# call after appmeta/manifest.json went up and before scripts/main.js did --
# leaving an app directory with no script in it, which the launcher shows as a
# menu that starts nothing. The device answers again within seconds, so a call
# that died on the wire is worth repeating before calling it a failure.
ATTEMPTS = 3
RETRY_WAIT = 3         # seconds before a retry, times the attempt just made


def _body(resp) -> str:
    """Read what the device said, and settle for the status if it cannot.

    A status line is the device's answer even when the body behind it arrives
    cut short, so the read is not allowed to take the answer down with it --
    a write that already returned 200 has been written, and throwing that away
    to ask again would undo it and write it a second time for nothing.
    """
    try:
        return resp.read().decode(errors="replace")
    except (OSError, http.client.HTTPException) as exc:
        return f"(body cut short: {exc})"


def _died_on_the_wire(exc: BaseException) -> bool:
    """True when a call was never answered, rather than answered unhappily.

    This only ever sees calls that produced no status line at all; a body
    that failed after one is `_body`'s business, not a death. urllib reports a
    timeout either directly or wrapped in a URLError, depending on whether it
    fell over connecting or mid-response, and a reply too mangled to parse a
    status out of is an http.client exception rather than an OSError.
    """
    reason = exc.reason if isinstance(exc, urllib.error.URLError) else exc
    if isinstance(reason, ConnectionError):
        # A connection that was made and then broke: reset by the device, or a
        # pipe that closed while a chunk was still going up, which is the most
        # likely way an 8 KB write dies. Refused is the one to let through --
        # that is a wrong address, and repeating it just delays saying so.
        return not isinstance(reason, ConnectionRefusedError)
    return isinstance(reason, (TimeoutError, http.client.HTTPException))


def _call(base: str, path: str, params: dict, data: bytes | None = None,
          method: str = "GET", attempts: int = ATTEMPTS) -> tuple[int, str]:
    """Make one storage call, repeating one that dies on the wire.

    A status of 0 means no attempt was ever answered; every caller already
    reads anything but 200 as a failure, so a device that stays silent still
    ends the run non-zero. A caller whose work a single call cannot be repeated
    inside -- an append partway through a file -- passes attempts=1 and repeats
    that larger unit itself.
    """
    url = f"{base.rstrip('/')}{path}?{urllib.parse.urlencode(params)}"
    for attempt in range(1, attempts + 1):
        req = urllib.request.Request(url, data=data, method=method)
        if data is not None:
            req.add_header("Content-Type", "application/octet-stream")
        try:
            with urllib.request.urlopen(req, timeout=TIMEOUT) as resp:
                return resp.status, _body(resp)
        except urllib.error.HTTPError as e:
            # An answer, just an unhappy one. Repeating it changes nothing.
            return e.code, _body(e)
        except (OSError, http.client.HTTPException) as exc:
            if not _died_on_the_wire(exc):
                raise
            if attempt == attempts:
                return 0, f"no answer from the device: {exc}"
            wait = RETRY_WAIT * attempt
            print(f"  {path} did not answer ({exc}); retrying in {wait}s "
                  f"(attempt {attempt + 1} of {attempts})")
            time.sleep(wait)


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


MENU_FLAG = "/ext/apps_data/apps_menu/js_apps_enabled"


def enable_menu(base: str) -> int:
    """Switch the apps menu from its placeholder to the real app list.

    `apps_menu_is_js_apps_enabled()` stats this file; without it the menu shows
    "More apps soon" and lists nothing, which reads exactly like a firmware
    that cannot list user apps at all. It can, once asked. Separate from
    install because it changes a device-wide setting, not just our app.
    """
    code, body = _call(base, "/api/storage/write", {"path": MENU_FLAG},
                       data=b"1", method="POST")
    # 508 is the device refusing to write over a file already there, which for
    # a flag is the state being asked for: the menu was enabled on an earlier
    # run, or a retry is looking at what the attempt before it wrote and never
    # got to hear about. Either way the flag is set, which is the whole job.
    print(f"menu flag {MENU_FLAG}: {code} {body.strip()}"
          + (" (already set)" if code == 508 else ""))
    return 0 if code in (200, 508) else 1


def _upload(base: str, remote: str, blob: bytes) -> tuple[int, str, int]:
    """Write one file to the device, repeating the whole file if a call dies.

    A retry cannot pick up where a dead call left off: the device may well have
    written the chunk it never answered for, and appending that chunk a second
    time would corrupt the file. So every attempt starts again from the remove,
    which is what makes a file safe to write twice at all. The calls below pass
    attempts=1 for the same reason -- the repeating happens out here.
    """
    for attempt in range(1, ATTEMPTS + 1):
        # The device will not write over an existing file -- it answers 508
        # "Failed to open file for writing", which reads like a disk fault and
        # is really "this path is taken". Clear it first; a missing file is a
        # fine outcome too, so only silence from the remove is worth acting on:
        # writing over a file the device never confirmed gone earns that 508.
        code, body = _call(base, "/api/storage/remove", {"path": remote},
                           method="DELETE", attempts=1)
        parts = 0
        if code != 0:
            code, body = 200, ""
            for offset in range(0, max(len(blob), 1), CHUNK):
                params = {"path": remote}
                if offset:
                    params["append"] = 1
                code, body = _call(base, "/api/storage/write", params,
                                   data=blob[offset:offset + CHUNK],
                                   method="POST", attempts=1)
                parts += 1
                if code != 200:
                    break
        # Only silence is worth another go; a real status is the device's answer.
        if code != 0 or attempt == ATTEMPTS:
            return code, body, parts
        wait = RETRY_WAIT * attempt
        print(f"  write {remote}: {body}; starting the file over in {wait}s "
              f"(attempt {attempt + 1} of {ATTEMPTS})")
        time.sleep(wait)


def _gave_up() -> int:
    """Report a device that has stopped answering, and stop asking.

    Nothing later in the run would go better: every remaining call would spend
    its own attempts and waits before saying the same thing, which on a tree
    this size is twenty-odd minutes of repeating a verdict the first exhausted
    call already reached.
    """
    print("\nThe device stopped answering, so the rest was not attempted. "
          "Check the bar is awake and run this again.")
    return 1


def install(base: str) -> int:
    if not SRC.is_dir():
        print(f"missing source tree: {SRC}")
        return 1

    stop_running(urllib.parse.urlparse(base).hostname or base)

    for d in (REMOTE_ROOT, f"{REMOTE_ROOT}/appmeta", f"{REMOTE_ROOT}/scripts"):
        code, body = _call(base, "/api/storage/mkdir", {"path": d}, method="POST")
        # An existing directory is a 400 here, which is success for our purpose.
        print(f"  mkdir {d}: {code}" + ("" if code == 200 else f" {body.strip()}"))
        if code == 0:
            return _gave_up()

    failures = 0
    for local in sorted(SRC.rglob("*")):
        if local.is_dir():
            continue
        remote = f"{REMOTE_ROOT}/{local.relative_to(SRC).as_posix()}"
        blob = local.read_bytes()
        code, body, parts = _upload(base, remote, blob)
        suffix = f" in {parts} chunks" if parts > 1 else ""
        print(f"  write {remote} ({len(blob)}B){suffix}: {code}"
              + ("" if code == 200 else f" {body.strip()}"))
        if code == 0:
            return _gave_up()
        failures += code != 200

    if failures:
        print(f"\n{failures} file(s) failed.")
        return 1
    print("\nInstalled. Launch it as a real app over the device's telnet CLI "
          "(port 23):\n\n"
          f"  loader open js_app_launcher {APP_ID}\n\n"
          "To have it listed in the Apps menu, run this once with "
          "--enable-menu.\n"
          "It pulls from the daemon, so set `busybar_serve = 10.0.4.21:8724` "
          "in\n~/.config/claude-usage-monitor/config and restart the daemon.")
    return 0


def remove(base: str) -> int:
    code, body = _call(base, "/api/storage/remove", {"path": REMOTE_ROOT},
                       method="DELETE")
    # A 400 is the device saying there is nothing at that path -- again the
    # state being asked for, and what a retry sees after an attempt whose
    # answer went missing. The app is gone either way.
    print(f"remove {REMOTE_ROOT}: {code} {body.strip()}"
          + (" (nothing there)" if code == 400 else ""))
    return 0 if code in (200, 400) else 1


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    url = args[0] if args else "http://10.0.4.20"
    if "--remove" in sys.argv:
        sys.exit(remove(url))
    if "--enable-menu" in sys.argv:
        sys.exit(enable_menu(url))
    sys.exit(install(url))
