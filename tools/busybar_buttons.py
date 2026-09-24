#!/usr/bin/env python3
"""Read the BUSY Bar's buttons and drive the Petmeter daemon.

    python3 tools/busybar_buttons.py [10.0.4.20] [http://10.0.4.21:8724]

The daemon has this same reader built in (`daemon/sinks/busybar_buttons.py`),
and on macOS it usually cannot use it: connecting to a LAN address needs Local
Network permission, a launchd job cannot raise that prompt, and the attempt
fails with `[Errno 65] No route to host` while the identical connect succeeds
from a terminal. **Run this from a terminal and it works**, because your
terminal already has the grant.

It does the same thing the built-in reader does -- holds one CLI session on
the bar running `input dump`, which reports presses even while our canvas has
swallowed them -- and posts each one to the daemon's control endpoint instead
of touching its state directly.
"""
import re
import socket
import sys
import time
import urllib.error
import urllib.request

CLI_PORT = 23
EVENT = re.compile(r"key: (InputKey\w+) type: (InputType\w+)")

# Short for the buttons so a long press stays free for something else; Press
# for the wheel, which only emits that.
ACTIONS = {
    ("InputKeyStart", "InputTypeShort"): "pause",
    ("InputKeyOk", "InputTypeShort"): "next",
    ("InputKeyUp", "InputTypePress"): "next",
    ("InputKeyDown", "InputTypePress"): "prev",
}


def send(daemon: str, action: str) -> None:
    try:
        with urllib.request.urlopen(f"{daemon}/control?do={action}", timeout=5) as r:
            print(f"  {action} -> {r.read().decode()}")
    except (urllib.error.URLError, TimeoutError) as exc:
        print(f"  {action} -> daemon unreachable: {exc}")


def session(bar: str, daemon: str) -> None:
    with socket.create_connection((bar, CLI_PORT), timeout=5) as cli:
        cli.settimeout(2)
        try:
            cli.recv(8192)                      # banner
        except socket.timeout:
            pass
        cli.sendall(b"input dump\r\n")
        print(f"reading {bar}:{CLI_PORT} — press something")
        cli.settimeout(None)
        buf = b""
        while True:
            chunk = cli.recv(4096)
            if not chunk:
                return
            buf += chunk
            while b"\n" in buf:
                line, _, buf = buf.partition(b"\n")
                m = EVENT.search(line.decode("latin-1", "replace"))
                if m:
                    action = ACTIONS.get((m.group(1), m.group(2)))
                    print(f"{m.group(1)} {m.group(2)}"
                          + (f"  -> {action}" if action else ""))
                    if action:
                        send(daemon, action)


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    bar = args[0] if args else "10.0.4.20"
    daemon = args[1] if len(args) > 1 else "http://10.0.4.21:8724"
    while True:
        try:
            session(bar, daemon)
        except KeyboardInterrupt:
            raise SystemExit(0)
        except OSError as exc:
            print(f"no CLI ({exc}); retrying in 5s")
        time.sleep(5)
