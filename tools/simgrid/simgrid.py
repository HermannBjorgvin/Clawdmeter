#!/usr/bin/env python3
"""Every Clawdmeter panel at once, on one clock, from one set of actions.

    simgrid.py build [--boards IDS]             build the simulator per panel size
    simgrid.py run SCRIPT [--boards IDS]        headless: run a script of actions, exit
    simgrid.py serve [--boards IDS] [--port N] [--paused] [--no-browser]
                                                the web GUI and the HTTP action API
    simgrid.py ACTION [ARGS...] [--port N]      run one action on a serving grid
    simgrid.py actions                          list the actions and their arguments
    simgrid.py boards                           list the panels

Actions take positional arguments in parameter order, or key=value:
    simgrid.py beat attention
    simgrid.py tap round 233 300 hold_ms=5000
    simgrid.py payload s=88 w=40
    simgrid.py shot /tmp/grid.png scale=0.5

A script is one action per line in the same form (# starts a comment):
    preset enterprise
    step 3000
    beat attention
    record /tmp/attention.gif fps=15
    step 6000
    record_stop
    shot /tmp/after.png

`run` and `serve --paused` move the clock only through `step` (and actions
that hold something down), so a script gives the same pictures every time.
`serve` runs the clock in real time while playing. See docs/sim-grid.md.
"""

import argparse
import json
import os
import shlex
import signal
import sys
import threading
import time
import urllib.error
import urllib.request
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from io import BytesIO

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import engine as eng  # noqa: E402
from profiles import PROFILES, select  # noqa: E402

DEFAULT_PORT = int(os.environ.get("SIMGRID_PORT", "8765"))
HERE = os.path.dirname(os.path.abspath(__file__))


def parse_action(words):
    """['tap', 'round', '233', 'y=300'] → ('tap', ['round', '233'], {'y': '300'})."""
    if not words:
        raise ValueError("empty action")
    args, kwargs = [], {}
    for w in words[1:]:
        if "=" in w and not w.startswith("{"):
            k, v = w.split("=", 1)
            kwargs[k] = v
        else:
            args.append(w)
    return words[0], args, kwargs


def run_script(engine, lines, log=print):
    """Run actions one per line; print what shot / record_stop / status return."""
    for n, raw in enumerate(lines, 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        action, args, kwargs = parse_action(shlex.split(line))
        try:
            result = eng.call(engine, action, args, kwargs)
        except Exception as e:
            raise SystemExit(f"line {n}: {line}\n  {e}")
        if action in ("shot", "record_stop", "status"):
            log(json.dumps(result))


# ─── web server ──────────────────────────────────────────────────────────────

class Clock(threading.Thread):
    """Moves the virtual clock with the wall clock while playing."""

    def __init__(self, engine, playing):
        super().__init__(daemon=True)
        self.engine, self.playing = engine, playing

    def run(self):
        last = time.monotonic()
        while True:
            time.sleep(1 / 30)
            now = time.monotonic()
            dt = min(int((now - last) * 1000), 100)   # don't chase a stall
            last = now
            if self.playing and dt > 0:
                self.engine.step(dt)


def make_handler(engine, clock):
    page = os.path.join(HERE, "index.html")

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def _send(self, code, body, ctype="application/json"):
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def _png(self, img):
            buf = BytesIO()
            img.save(buf, "PNG", compress_level=1)
            self._send(200, buf.getvalue(), "image/png")

        def do_GET(self):
            path = self.path.split("?")[0]
            try:
                if path == "/":
                    with open(page, "rb") as f:
                        self._send(200, f.read(), "text/html; charset=utf-8")
                elif path == "/api/status":
                    st = engine.status()
                    st["playing"] = clock.playing
                    self._send(200, json.dumps(st).encode())
                elif path == "/api/actions":
                    d = {k: {"params": [p.name for p in ps], "doc": doc}
                         for k, (ps, doc) in eng.describe().items()}
                    self._send(200, json.dumps(d).encode())
                elif path.startswith("/frame/") and path.endswith(".png"):
                    self._png(engine.frame(path[len("/frame/"):-4]))
                elif path == "/grid.png":
                    self._png(engine.grid(0.5))
                else:
                    self._send(404, b'{"error":"not found"}')
            except Exception as e:
                self._send(400, json.dumps({"error": str(e)}).encode())

        def do_POST(self):
            path = self.path.split("?")[0]
            n = int(self.headers.get("Content-Length") or 0)
            try:
                body = json.loads(self.rfile.read(n) or b"{}")
                if path in ("/api/play", "/api/pause"):
                    clock.playing = path == "/api/play"
                    result = {"playing": clock.playing}
                elif path.startswith("/api/"):
                    action = path[len("/api/"):]
                    result = eng.call(engine, action, body.get("args", []), body.get("kwargs", {}))
                else:
                    raise ValueError("not found")
                self._send(200, json.dumps(result).encode())
            except Exception as e:
                self._send(400, json.dumps({"error": str(e)}).encode())

    return Handler


def _interrupt(*_):
    raise KeyboardInterrupt


def serve(args):
    engine = eng.Engine(args.boards)
    clock = Clock(engine, playing=not args.paused)
    clock.start()
    server = ThreadingHTTPServer(("127.0.0.1", args.port), make_handler(engine, clock))
    url = f"http://127.0.0.1:{args.port}/"
    print(f"simgrid: {len(engine.sims)} boards at {url} "
          f"({'paused' if args.paused else 'real time'}); Ctrl-C to stop", flush=True)
    if not args.no_browser:
        threading.Timer(0.5, lambda: webbrowser.open(url)).start()
    signal.signal(signal.SIGTERM, _interrupt)   # a plain kill shuts the sims down too
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        engine.close()


def client(action, words, port):
    """One action on a serving grid, over HTTP."""
    _, args, kwargs = parse_action([action] + words)
    if action in ("play", "pause"):
        url, body = f"http://127.0.0.1:{port}/api/{action}", {}
    else:
        url, body = f"http://127.0.0.1:{port}/api/{action}", {"args": args, "kwargs": kwargs}
    if action == "status":
        req = urllib.request.Request(f"http://127.0.0.1:{port}/api/status")
    else:
        req = urllib.request.Request(url, data=json.dumps(body).encode(),
                                     headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req) as r:
            print(r.read().decode())
    except urllib.error.HTTPError as e:
        raise SystemExit(e.read().decode())
    except urllib.error.URLError:
        raise SystemExit(f"no grid serving on port {port}; start one with `simgrid.py serve`")


def main():
    argv = sys.argv[1:]
    if not argv or argv[0] in ("-h", "--help", "help"):
        print(__doc__)
        return
    cmd = argv[0]
    if cmd in ("build", "run", "serve"):
        ap = argparse.ArgumentParser(prog=f"simgrid.py {cmd}")
        if cmd == "run":
            ap.add_argument("script", help="script file, or - for stdin")
        ap.add_argument("--boards", default=None,
                        help="comma list of board ids or sizes, or `main` (default: all)")
        if cmd == "serve":
            ap.add_argument("--port", type=int, default=DEFAULT_PORT)
            ap.add_argument("--paused", action="store_true",
                            help="start with the clock stopped (it moves on `step` / play)")
            ap.add_argument("--no-browser", action="store_true")
        if cmd == "build":
            ap.add_argument("--jobs", type=int, default=4)
        args = ap.parse_args(argv[1:])
        if cmd == "build":
            eng.build(select(args.boards), args.jobs)
        elif cmd == "serve":
            eng.build(select(args.boards))
            serve(args)
        else:
            eng.build(select(args.boards))
            engine = eng.Engine(args.boards)
            try:
                lines = sys.stdin if args.script == "-" else open(args.script)
                run_script(engine, lines)
            finally:
                engine.close()
        return
    if cmd == "actions":
        for name, (params, doc) in eng.describe().items():
            sig = " ".join(p.name if p.default is p.empty else f"[{p.name}={p.default!r}]"
                           for p in params if p.kind != p.VAR_KEYWORD)
            if any(p.kind == p.VAR_KEYWORD for p in params):
                sig += " [field=value ...]"
            print(f"{name} {sig}\n    {doc}")
        print("play / pause\n    Start or stop the real-time clock of a serving grid.")
        return
    if cmd == "boards":
        for p in PROFILES:
            print(f"{p.id:9} {p.label}{'' if p.on_main else '  (port PR)'}")
        return
    port = DEFAULT_PORT
    if "--port" in argv:
        i = argv.index("--port")
        port = int(argv[i + 1])
        argv = argv[:i] + argv[i + 2:]
    client(cmd, argv[1:], port)


if __name__ == "__main__":
    main()
