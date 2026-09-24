#!/usr/bin/env python3
"""Let the launchd daemon reach the BUSY Bar over the LAN, on macOS.

    python3 tools/busybar_lan_access.py [--revert] [--label <launchd label>]

THE PROBLEM. Since Sequoia, macOS gates LAN traffic behind Local Network
privacy, and it attributes a socket to *the executable launchd spawned*. A
venv python has no application identity to attribute, so it is denied -- and
because a background job cannot raise the prompt, it is denied **silently**.
The connect fails with `[Errno 65] No route to host`, which reads as "that
host is down" and is really "the OS blocked you". Granting "Python" in System
Settings does not help: the entry is not the identity being consulted. Neither
does a reboot. Both were tried on real hardware.

THE FIX. `osascript` is an Apple platform binary, and `do shell script` makes
its child osascript-responsible, which carries the exemption. So the daemon is
spawned through it. Measured A/B on the same machine, same probe:

    launchd -> python             [Errno 65] No route to host
    launchd -> osascript -> python   connected

Bluetooth still works through this path, which matters more than the buttons:
the daemon needs launchd for CoreBluetooth (it SIGABRTs from a plain shell),
so any fix that cost BLE would be a bad trade. Verified -- the desk meter kept
receiving data across the change.

This patches the INSTALLED LaunchAgent, not the repo's template, which belongs
to upstream: editing that would cost a merge conflict on every sync for a fix
only this fork needs.
"""
import plistlib
import subprocess
import sys
from pathlib import Path

LABEL = "com.user.claude-usage-daemon"
OSASCRIPT = "/usr/bin/osascript"


def plist_path(label: str) -> Path:
    return Path.home() / "Library" / "LaunchAgents" / f"{label}.plist"


def wrapped(args: list[str], out: str, err: str) -> list[str]:
    # Redirection is explicit because `do shell script` buffers its child's
    # output until exit -- without it the log stops updating until the daemon
    # stops, which looks exactly like a hang.
    cmd = " ".join(args)
    return [OSASCRIPT, "-e", f'do shell script "exec {cmd} >> {out} 2>> {err}"']


def unwrap(args: list[str]) -> list[str] | None:
    """The original argv from a wrapped ProgramArguments, or None."""
    if len(args) != 3 or args[0] != OSASCRIPT:
        return None
    inner = args[2]
    start = inner.find('"exec ') + len('"exec ')
    end = inner.find(" >>", start)
    return inner[start:end].split() if start > 5 and end > start else None


def main() -> int:
    label = LABEL
    if "--label" in sys.argv:
        label = sys.argv[sys.argv.index("--label") + 1]
    path = plist_path(label)
    if not path.exists():
        print(f"no LaunchAgent at {path}")
        return 1

    data = plistlib.loads(path.read_bytes())
    args = data.get("ProgramArguments") or []
    logs = Path.home() / "Library" / "Logs"
    out = str(logs / f"{label.split('.')[-1]}.out.log")
    err = str(logs / f"{label.split('.')[-1]}.err.log")
    original = unwrap(args)

    if "--revert" in sys.argv:
        if original is None:
            print("not wrapped; nothing to revert")
            return 0
        data["ProgramArguments"] = original
        data["StandardOutPath"], data["StandardErrorPath"] = out, err
        print(f"reverted to: {' '.join(original)}")
    else:
        if original is not None:
            print("already wrapped")
            return 0
        # The daemon writes its own log lines to stdout, so redirect there and
        # drop the plist's paths -- they would now capture osascript's output,
        # not the daemon's.
        out = data.get("StandardOutPath", out)
        err = data.get("StandardErrorPath", err)
        data["ProgramArguments"] = wrapped(args, out, err)
        data.pop("StandardOutPath", None)
        data.pop("StandardErrorPath", None)
        print(f"wrapped: {' '.join(args)}")

    path.write_bytes(plistlib.dumps(data))
    uid = subprocess.run(["id", "-u"], capture_output=True, text=True).stdout.strip()
    print("\nRestart it (bootout must finish before bootstrap, or you get "
          "I/O error 5):\n"
          f"  launchctl bootout gui/{uid}/{label}\n"
          f"  launchctl bootstrap gui/{uid} {path}\n\n"
          "Then look for 'busybar buttons: reading' in the daemon log.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
