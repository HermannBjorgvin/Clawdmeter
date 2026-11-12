"""The grid's engine: one simulator process per panel, all on one virtual
clock, fed one payload stream and one set of commands.

Each simulator runs in controller mode (firmware/src/boards/sim/sim_ctrl.cpp):
its time moves only when the engine advances it, so a sequence of actions
gives the same frames every run. The engine plays the desktop app's part
(full payloads, a heartbeat every 60 s, the clock), forwards serial commands
such as `clawd attention`, routes taps and button presses to one board, and
turns frames into PNGs, GIFs and MP4s.

Every public method listed in ACTIONS is an action: the CLI, scripts and the
web GUI all call them by name with keyword arguments (simgrid.py).
"""

import copy
import inspect
import json
import os
import shutil
import socket
import subprocess
import threading
from concurrent.futures import ThreadPoolExecutor

from PIL import Image, ImageDraw, ImageFont

from profiles import select

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
FIRMWARE = os.path.join(REPO, "firmware")
BUILD_ROOT = os.path.join(FIRMWARE, ".pio", "simgrid")

# The clock starts here (local wall-clock seconds, as the app sends `t`):
# 09:41 on a Thursday.
CLOCK_EPOCH = 1785490860
HEARTBEAT_MS = 60000          # the app's replay interval (rp:1)
ONE_SHOT = ("b", "m", "x", "c")   # payload keys that act once, never repeated

USAGE = {"s": 42.0, "sr": 175, "w": 33.0, "wr": 7900, "st": "allowed", "ok": True}
PRESETS = {
    "usage": dict(USAGE),
    "fresh": {"s": 2.0, "sr": 298, "w": 33.0, "wr": 7900, "st": "allowed", "ok": True},
    "high": {"s": 78.0, "sr": 75, "w": 55.0, "wr": 6400, "st": "allowed", "ok": True},
    "maxed": {"s": 100.0, "sr": 12, "w": 96.0, "wr": 300, "st": "limited", "ok": True},
    "enterprise": {"s": 88.0, "sr": -1, "acct": "ent", "tp": 52, "pd": 30, "rd": "Aug 15",
                   "st": "allowed", "ok": True},
    "not-started": {"s": 0.0, "sr": -1, "w": 33.0, "wr": 7900, "st": "allowed", "ok": True},
    "nodata": {"ok": False},
}


def program_path(size):
    return os.path.join(BUILD_ROOT, size, "sim", "program")


def _newest_source():
    """mtime of the newest file a sim build depends on."""
    newest = os.path.getmtime(os.path.join(FIRMWARE, "platformio.ini"))
    for root, _, files in os.walk(os.path.join(FIRMWARE, "src")):
        for f in files:
            newest = max(newest, os.path.getmtime(os.path.join(root, f)))
    return newest


def build(profiles, jobs=4, log=print):
    """Build the simulator once per panel size the profiles need (incremental)."""
    pio = shutil.which("pio") or os.path.expanduser("~/.platformio/penv/bin/pio")
    newest = _newest_source()
    sizes = sorted({p.size for p in profiles
                    if not os.path.exists(program_path(p.size))
                    or os.path.getmtime(program_path(p.size)) < newest})
    if not sizes:
        return

    def one(size):
        w, h = size.split("x")
        env = dict(os.environ, PLATFORMIO_BUILD_DIR=os.path.join(BUILD_ROOT, size),
                   PLATFORMIO_BUILD_FLAGS=f"-DSIM_LCD_W={w} -DSIM_LCD_H={h}")
        r = subprocess.run([pio, "run", "-e", "sim"], cwd=FIRMWARE, env=env,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        return size, r.returncode, r.stdout

    with ThreadPoolExecutor(jobs) as pool:
        for size, rc, out in pool.map(one, sizes):
            if rc:
                raise RuntimeError(f"sim build {size} failed:\n{out[-3000:]}")
            # pio skips the link when a change doesn't alter any object (a
            # comment in a header), so stamp the binary as current.
            os.utime(program_path(size))
    log(f"simgrid: built the sim for {', '.join(sizes)}")


class Sim:
    """One simulator process in controller mode."""

    def __init__(self, profile, log_dir):
        self.profile = profile
        program = program_path(profile.size)
        if not os.path.exists(program):
            raise RuntimeError(f"no sim build for {profile.size}; run `simgrid.py build`")
        ours, theirs = socket.socketpair()
        env = dict(os.environ, SDL_VIDEODRIVER="dummy", SIM_CTRL_FD=str(theirs.fileno()),
                   **profile.env())
        self.log = open(os.path.join(log_dir, f"{profile.id}.log"), "w")
        self.proc = subprocess.Popen([program], cwd=FIRMWARE, env=env, pass_fds=[theirs.fileno()],
                                     stdin=subprocess.DEVNULL, stdout=self.log, stderr=subprocess.STDOUT)
        theirs.close()
        self.sock = ours
        self.io = ours.makefile("rwb")
        self.version = -1
        self.frame = None           # PIL RGB image, brightness applied
        self.frame_version = -2
        hello = self.request("hello").split()
        w, h = int(hello[1]), int(hello[2])
        if (w, h) != (profile.w, profile.h):
            raise RuntimeError(f"{profile.id}: the {profile.size} build reports {w}x{h}")

    def send(self, line):
        self.io.write(line.encode() + b"\n")
        self.io.flush()

    def recv(self):
        line = self.io.readline().decode().strip()
        if not line:
            raise RuntimeError(f"{self.profile.id}: simulator exited (log: {self.log.name})")
        if line.startswith("err"):
            raise RuntimeError(f"{self.profile.id}: {line}")
        return line

    def request(self, line):
        self.send(line)
        return self.recv()

    def fetch_frame(self):
        """The panel as last flushed, if it changed since the last fetch."""
        if self.frame is not None and self.frame_version == self.version:
            return self.frame
        self.send("frame")
        head = self.recv().split()
        w, h, bright, version, n = (int(x) for x in head[1:6])
        data = self.io.read(n)
        img = Image.frombytes("RGB", (w, h), data, "raw", "BGR;16")
        if bright < 255:
            img = img.point(lambda v: v * bright // 255)
        self.frame, self.frame_version, self.version = img, version, version
        return img

    def close(self):
        try:
            self.request("quit")
        except Exception:
            pass
        self.proc.wait(timeout=5)
        self.log.close()


class Recorder:
    def __init__(self, path, board, fps, scale):
        self.path, self.board, self.fps, self.scale = path, board, fps, scale
        self.frames = []

    def save(self):
        if not self.frames:
            raise RuntimeError("nothing recorded")
        os.makedirs(os.path.dirname(os.path.abspath(self.path)), exist_ok=True)
        if self.path.endswith(".gif"):
            first, rest = self.frames[0], self.frames[1:]
            first.save(self.path, save_all=True, append_images=rest, loop=0,
                       duration=round(1000 / self.fps), optimize=True)
        elif self.path.endswith(".mp4"):
            ffmpeg = shutil.which("ffmpeg")
            if not ffmpeg:
                raise RuntimeError("recording .mp4 needs ffmpeg on PATH (or record a .gif)")
            w, h = self.frames[0].size
            w2, h2 = w + w % 2, h + h % 2
            p = subprocess.Popen([ffmpeg, "-y", "-loglevel", "error", "-f", "rawvideo",
                                  "-pix_fmt", "rgb24", "-s", f"{w2}x{h2}", "-r", str(self.fps),
                                  "-i", "-", "-pix_fmt", "yuv420p", "-c:v", "libx264", self.path],
                                 stdin=subprocess.PIPE)
            for f in self.frames:
                if f.size != (w2, h2):
                    padded = Image.new("RGB", (w2, h2))
                    padded.paste(f)
                    f = padded
                p.stdin.write(f.tobytes())
            p.stdin.close()
            if p.wait():
                raise RuntimeError("ffmpeg failed")
        else:
            raise RuntimeError("record to a .gif or .mp4 path")
        return len(self.frames)


def _font(size):
    for f in ("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
              "/System/Library/Fonts/Supplemental/Arial.ttf", "C:/Windows/Fonts/arial.ttf"):
        if os.path.exists(f):
            return ImageFont.truetype(f, size)
    return ImageFont.load_default()


def _fit(draw, text, font, width):
    """`text`, cut with an ellipsis to fit `width` pixels."""
    if draw.textlength(text, font=font) <= width:
        return text
    while text and draw.textlength(text + "…", font=font) > width:
        text = text[:-1]
    return text + "…"


class Engine:
    """All boards, one clock. Every action holds `lock`, so the web server's
    threads and its real-time ticker never interleave."""

    def __init__(self, boards=None, log_dir=None, data=True):
        self.lock = threading.RLock()
        self.profiles = select(boards)
        self.log_dir = log_dir or os.path.join(BUILD_ROOT, "logs")
        os.makedirs(self.log_dir, exist_ok=True)
        with ThreadPoolExecutor(len(self.profiles)) as pool:
            self.sims = list(pool.map(lambda p: Sim(p, self.log_dir), self.profiles))
        self.now = 0
        self.state = dict(USAGE, lv=3, tf=24)
        self.connected = True
        self.heartbeat = True
        self.battery_state = (87, False, False)
        self.es = None              # last hook-event sequence sent
        self.timers = []            # (due_ms, seq, fn) run as the clock passes them
        self._seq = 0
        self.last_sent = None
        self.scenario_states = []   # [(name, payload, hold_ms)]
        self._scene_token = None
        self.scene = -1
        self.scene_playing = False
        self.recorder = None
        self._layout = None
        self.battery(87, False, False)
        if data:
            self._send()

    # ─── plumbing ────────────────────────────────────────────────────────────

    def _broadcast(self, line, sims=None):
        sims = sims or self.sims
        for s in sims:
            s.send(line)
        return [s.recv() for s in sims]

    def _sim(self, board):
        for s in self.sims:
            if str(board) in (s.profile.id, s.profile.size):
                return s
        raise ValueError(f"unknown board {board!r}; running: {', '.join(s.profile.id for s in self.sims)}")

    def _at(self, due, fn):
        self._seq += 1
        self.timers.append((due, self._seq, fn))
        self.timers.sort()

    def _send(self, extra=None, replay=False):
        """One full payload to every board, as the app writes it."""
        if not self.connected:
            return
        p = dict(self.state)
        p["t"] = CLOCK_EPOCH + self.now // 1000
        if replay:
            p["rp"] = 1
        if extra:
            p.update(extra)
        self._broadcast("payload " + json.dumps(p, separators=(",", ":")))
        self.last_sent = self.now
        if not replay:
            self._schedule_heartbeat()

    def _schedule_heartbeat(self):
        sent = self.last_sent

        def beat():
            if self.heartbeat and self.last_sent == sent and self.state.get("ok", True):
                self._send(replay=True)
                self._schedule_heartbeat()
        self._at(self.now + HEARTBEAT_MS, beat)

    def _advance(self, ms):
        if ms <= 0:
            return
        for s, reply in zip(self.sims, self._broadcast(f"advance {ms}")):
            s.version = int(reply.split()[2])
        self.now += ms

    # ─── time ────────────────────────────────────────────────────────────────

    def step(self, ms=1000):
        """Advance every board's clock by `ms`, running whatever falls due on
        the way (scenario changes, heartbeats, recorded frames, releases)."""
        with self.lock:
            end = self.now + int(ms)
            while True:
                due = self.timers[0][0] if self.timers else None
                if due is None or due > end:
                    break
                self._advance(due - self.now)
                _, _, fn = self.timers.pop(0)
                fn()
            self._advance(end - self.now)
            return {"now": self.now}

    # ─── the app's side: payloads ────────────────────────────────────────────

    def payload(self, replace=False, **fields):
        """Set usage fields (s sr w wr st acct tp pd rd ok lv tf n a ...) and
        send. b/m/x/c act once. replace=true starts from an empty payload."""
        with self.lock:
            once = {k: fields.pop(k) for k in ONE_SHOT if k in fields}
            if replace:
                self.state = {k: v for k, v in self.state.items() if k in ("lv", "tf", "pm")}
            self.state.update(fields)
            self._send(once)
            return {"payload": self.state}

    def preset(self, name="usage"):
        """Usage presets: usage, fresh, high, maxed, enterprise, not-started, nodata."""
        if name not in PRESETS:
            raise ValueError(f"presets: {', '.join(PRESETS)}")
        with self.lock:
            keep = {k: self.state[k] for k in ("lv", "tf", "pm", "a", "e", "es", "n") if k in self.state}
            self.state = dict(PRESETS[name], **keep)
            self._send({"c": True} if name == "fresh" else None)
            return {"payload": self.state}

    def stale(self, on=True):
        """Stop the heartbeat (on) so the boards' data goes stale after 90 s,
        or resume it."""
        with self.lock:
            self.heartbeat = not on
            if not on:
                self._send()
            return {"heartbeat": self.heartbeat}

    def alert(self, message="Claude needs your permission", chime=True):
        """Attention: the message on every board (and the alert chime)."""
        with self.lock:
            self._send({"m": message, "b": bool(chime)})
            return {}

    def clear(self):
        """End the attention alert."""
        with self.lock:
            self._send({"x": True})
            return {}

    def activity(self, level=3):
        """Activity level 0-3 (a)."""
        return self.payload(a=int(level))

    def event(self, kind="p"):
        """A hook event: p (prompt), s (stop) or b."""
        with self.lock:
            # Like the app: an event goes out ahead of the poll, so its usage
            # numbers are the last poll's and the write is marked rp:1.
            if self.es is None:          # the first sequence is a baseline
                self.es = 9000
                self.state.update(e=kind, es=self.es)
                self._send({"rp": 1})
            self.es += 1
            self.state.update(e=kind, es=self.es)
            self._send({"rp": 1})
            return {"es": self.es}

    def sessions(self, n=1):
        """Active Claude Code sessions (n)."""
        return self.payload(n=int(n))

    def liveliness(self, level=3):
        """Clawd's liveliness 1-5 (lv)."""
        return self.payload(lv=int(level))

    def pace_marker(self, on=True):
        """The pace marker on the usage bars (pm)."""
        return self.payload(pm=int(bool(on)))

    def clock(self, fmt=24):
        """Clock format, 12 or 24 (tf)."""
        return self.payload(tf=int(fmt))

    def link(self, on=True):
        """The BLE link up or down on every board."""
        with self.lock:
            self.connected = bool(on)
            self._broadcast(f"link {int(self.connected)}")
            if self.connected:
                self._send()
            return {"link": self.connected}

    def battery(self, pct=87, charging=False, usb=False):
        """Battery percentage, charging, and USB-only (no battery) on every board."""
        with self.lock:
            self.battery_state = (int(pct), bool(charging), bool(usb))
            self._broadcast(f"battery {int(pct)} {int(bool(charging))} {int(bool(usb))}")
            return {"battery": self.battery_state}

    # ─── scenarios ───────────────────────────────────────────────────────────

    def scenario(self, path="sim/scenario.jsonl", play=True):
        """Load a scenario (sim JSONL: one payload per line, optional name and
        hold_ms) and play it from the top."""
        with self.lock:
            full = path if os.path.isabs(path) else os.path.join(FIRMWARE, path)
            if not os.path.exists(full):
                full = os.path.abspath(path)
            states = []
            with open(full) as f:
                for line in f:
                    line = line.strip()
                    if not line or line.startswith("#"):
                        continue
                    d = json.loads(line)
                    name = d.pop("name", f"state {len(states) + 1}")
                    hold = int(d.pop("hold_ms", 3000))
                    states.append((name, d, hold))
            self.scenario_states = states
            self.scene = -1
            self.scene_playing = False
            if play:
                self.scenario_play()
            return {"states": [s[0] for s in states]}

    def _scene(self, i):
        name, d, hold = self.scenario_states[i]
        self.scene = i
        d = copy.deepcopy(d)
        once = {k: d.pop(k) for k in ONE_SHOT if k in d}
        keep = {k: self.state[k] for k in ("lv", "tf", "pm", "a", "e", "es", "n") if k in self.state}
        self.state = dict(d, **{k: v for k, v in keep.items() if k not in d})
        self._send(once)
        if self.scene_playing:
            token = (i, self.now)
            self._scene_token = token

            def nxt():
                if self.scene_playing and self._scene_token == token:
                    self._scene((i + 1) % len(self.scenario_states))
            self._at(self.now + hold, nxt)

    def scenario_play(self):
        """Play the loaded scenario from the current state."""
        with self.lock:
            self.scene_playing = True
            self._scene(max(self.scene, 0))
            return {"scene": self.scenario_states[self.scene][0]}

    def scenario_pause(self):
        """Hold the current scenario state."""
        with self.lock:
            self.scene_playing = False
            return {"scene": self.scene}

    def scenario_step(self, by=1):
        """Show the next (or by=-1 previous) scenario state, paused."""
        with self.lock:
            self.scene_playing = False
            self._scene((self.scene + int(by)) % len(self.scenario_states))
            return {"scene": self.scenario_states[self.scene][0]}

    # ─── Clawd and the serial console ────────────────────────────────────────

    def beat(self, name="attention", arg=""):
        """Force a Clawd beat on every board: show, screensaver, attention,
        joy, lurk, home, peek [x]."""
        return self.serial(f"clawd {name} {arg}".strip())

    def serial(self, line, board=""):
        """Type a serial command on every board (or one)."""
        with self.lock:
            sims = [self._sim(board)] if board else None
            self._broadcast(f"cmd {line}", sims)
            return {}

    # ─── one board's inputs ──────────────────────────────────────────────────

    def touch(self, board, x, y, down=True):
        """Finger down (or up) on one board, in its panel's pixels."""
        with self.lock:
            self._sim(board).request(f"touch {int(x)} {int(y)} {int(bool(down))}")
            return {}

    def tap(self, board, x, y, hold_ms=120):
        """Touch one board at x,y for hold_ms (a hold when long), advancing the clock."""
        with self.lock:
            self.touch(board, x, y, True)
            self.step(hold_ms)
            self.touch(board, x, y, False)
            return {"now": self.now}

    def button(self, board, name, down=True):
        """Hold (or release) one board's button: boot, secondary or pwr."""
        with self.lock:
            self._sim(board).request(f"button {name} {int(bool(down))}")
            return {}

    def press(self, board, name="boot", hold_ms=100):
        """Press one board's button for hold_ms, advancing the clock (pwr
        held ~3 s and released = pair)."""
        with self.lock:
            self.button(board, name, True)
            self.step(hold_ms)
            self.button(board, name, False)
            return {"now": self.now}

    # ─── pictures ────────────────────────────────────────────────────────────

    def frame(self, board):
        """One board's panel as a PIL image (not an action)."""
        with self.lock:
            return self._sim(board).fetch_frame()

    def _grid_layout(self, scale):
        """Cell positions: rows packed to ~1900 px (at scale 1), labels above."""
        key = scale
        if self._layout and self._layout[0] == key:
            return self._layout[1]
        pad, label_h, max_w = int(16 * scale) or 1, int(40 * scale) + 6, int(1900 * scale)
        cells, x, y, row_h = [], pad, pad, 0
        for s in self.sims:
            w, h = round(s.profile.w * scale), round(s.profile.h * scale)
            if x > pad and x + w > max_w:
                x, y, row_h = pad, y + row_h + pad, 0
            cells.append((s, x, y, w, h))
            x += w + pad
            row_h = max(row_h, label_h + h)
        size = (max(c[1] + c[3] for c in cells) + pad, y + row_h + pad)
        self._layout = (key, (cells, size, label_h))
        return self._layout[1]

    def _panel(self, sim, w, h):
        img = sim.fetch_frame()
        if img.size != (w, h):
            img = img.resize((w, h), Image.LANCZOS)
        if sim.profile.round:
            mask = Image.new("L", (w, h), 0)
            ImageDraw.Draw(mask).ellipse((0, 0, w - 1, h - 1), fill=255)
            bg = Image.new("RGB", (w, h), (28, 28, 28))
            bg.paste(img, (0, 0), mask)
            img = bg
        return img

    def grid(self, scale=1.0):
        """Every board in one labelled image (not an action)."""
        with self.lock:
            cells, size, label_h = self._grid_layout(scale)
            out = Image.new("RGB", size, (28, 28, 28))
            d = ImageDraw.Draw(out)
            font = _font(max(9, int(15 * scale)))
            small = _font(max(8, int(13 * scale)))
            for s, x, y, w, h in cells:
                p = s.profile
                d.text((x, y + 1), _fit(d, f"{p.w}×{p.h}{' round' if p.round else ''} · {p.id}", font, w),
                       font=font, fill=(225, 225, 220))
                d.text((x, y + 2 + int(19 * scale)), _fit(d, ", ".join(p.boards), small, w),
                       font=small, fill=(150, 150, 145))
                out.paste(self._panel(s, w, h), (x, y + label_h))
            return out

    def shot(self, path, board="", scale=1.0):
        """Save the grid (or one board) as a PNG."""
        with self.lock:
            img = self._panel(self._sim(board), *self._sim(board).fetch_frame().size) \
                if board else self.grid(float(scale))
            if board and float(scale) != 1.0:
                img = img.resize((round(img.width * float(scale)), round(img.height * float(scale))),
                                 Image.LANCZOS)
            os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
            img.save(path)
            return {"path": os.path.abspath(path), "size": img.size}

    def record(self, path, board="", fps=20, scale=0.5):
        """Start recording the grid (or one board) to a .gif or .mp4, one
        frame every 1000/fps ms of clock; `record_stop` writes it."""
        with self.lock:
            if self.recorder:
                raise RuntimeError("already recording")
            rec = Recorder(path, board, int(fps), float(scale))
            self.recorder = rec
            interval = 1000 / rec.fps

            def capture(n=0):
                if self.recorder is not rec:
                    return
                if rec.board:
                    img = self._panel(self._sim(rec.board), *self._sim(rec.board).fetch_frame().size)
                    if rec.scale != 1.0:
                        img = img.resize((round(img.width * rec.scale), round(img.height * rec.scale)),
                                         Image.LANCZOS)
                else:
                    img = self.grid(rec.scale)
                rec.frames.append(img)
                self._at(start + round((n + 1) * interval), lambda: capture(n + 1))
            start = self.now
            capture()
            return {"recording": path}

    def record_stop(self):
        """Stop recording and write the file."""
        with self.lock:
            rec, self.recorder = self.recorder, None
            if not rec:
                raise RuntimeError("not recording")
            n = rec.save()
            return {"path": os.path.abspath(rec.path), "frames": n}

    # ─── state ───────────────────────────────────────────────────────────────

    def status(self):
        """Clock, payload, link, battery, scenario, recording and every board."""
        with self.lock:
            return {
                "now": self.now,
                "payload": self.state,
                "link": self.connected,
                "heartbeat": self.heartbeat,
                "battery": self.battery_state,
                "scenario": {"states": [s[0] for s in self.scenario_states],
                             "scene": self.scene, "playing": self.scene_playing},
                "recording": self.recorder.path if self.recorder else None,
                "boards": [{"id": s.profile.id, "label": s.profile.label, "w": s.profile.w,
                            "h": s.profile.h, "round": s.profile.round, "pwr": s.profile.pwr,
                            "buttons": s.profile.buttons, "minimal": s.profile.minimal,
                            "on_main": s.profile.on_main, "version": s.version}
                           for s in self.sims],
            }

    def close(self):
        for s in self.sims:
            s.close()


# Actions callable by name from the CLI, scripts and the web GUI.
ACTIONS = ("step", "payload", "preset", "stale", "alert", "clear", "activity", "event",
           "sessions", "liveliness", "pace_marker", "clock", "link", "battery", "scenario", "scenario_play",
           "scenario_pause", "scenario_step", "beat", "serial", "touch", "tap", "button",
           "press", "shot", "record", "record_stop", "status")


def describe():
    """Action name → (parameters, first docstring paragraph), for help."""
    out = {}
    for name in ACTIONS:
        fn = getattr(Engine, name)
        params = [p for p in inspect.signature(fn).parameters.values() if p.name != "self"]
        out[name] = (params, " ".join((fn.__doc__ or "").split()))
    return out


def call(engine, action, args=(), kwargs=None):
    """Run an action with positional and keyword arguments (strings from the
    CLI are coerced to the parameter's default type)."""
    if action not in ACTIONS:
        raise ValueError(f"unknown action {action!r}; actions: {', '.join(ACTIONS)}")
    fn = getattr(engine, action)
    params = [p for p in inspect.signature(fn).parameters.values()]
    named = [p for p in params if p.kind in (p.POSITIONAL_OR_KEYWORD, p.KEYWORD_ONLY)]
    kw = dict(kwargs or {})
    for p, v in zip(named, args):
        kw[p.name] = v
    for p in named:
        if p.name in kw and isinstance(kw[p.name], str):
            kw[p.name] = coerce(kw[p.name], p.default)
    if any(p.kind == p.VAR_KEYWORD for p in params):
        for k, v in list(kw.items()):
            if isinstance(v, str) and k not in {p.name for p in named}:
                kw[k] = coerce(v, None)
    return fn(**kw)


def coerce(text, default):
    """A CLI string as the type the parameter expects."""
    if isinstance(default, bool):
        return text.lower() in ("1", "true", "yes", "on")
    if isinstance(default, int) and not isinstance(default, bool):
        return int(text)
    if isinstance(default, float):
        return float(text)
    if default is None or default is inspect.Parameter.empty:
        try:
            return json.loads(text)
        except ValueError:
            return text
    return text
