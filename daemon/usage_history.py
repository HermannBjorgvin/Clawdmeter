#!/usr/bin/env python3
"""Local-transcript usage history for the device's History screen.

Claude Code writes one JSONL transcript per session under
``<config_dir>/projects/<project>/*.jsonl``; every assistant turn carries
``message.usage`` (input / output / cache read / cache write tokens) plus the
model and a UTC timestamp. That's everything a per-day usage plot needs, and
it's already on disk — no API call, no token.

This module tails those files *incrementally*: per-file byte offsets are kept
so a poll only reads what was appended since the last one (a full corpus is
gigabytes; re-reading it every 60 s is not an option). Turns are dedup'd by
``(requestId, message.id)`` because Claude Code emits one line per content
block, each repeating the same usage. Days are bucketed in *local* time.

Standalone use::

    python -m daemon.usage_history               # table for the last 14 days
    python -m daemon.usage_history --json        # the payload fields
    python -m daemon.usage_history --no-state    # ignore/skip the state file

The daemon calls :meth:`UsageHistory.scan` each poll (in a thread) and merges
:meth:`UsageHistory.payload_fields` into the BLE payload.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time
from dataclasses import dataclass, field
from datetime import datetime, timedelta, timezone, tzinfo
from pathlib import Path
from typing import Callable, Iterable

# Payload keys (kept short: the whole BLE payload has to fit the device's
# 512-byte RX buffer alongside the existing usage fields).
#   h   per-day output tokens, in thousands, oldest → newest (today last)
#   ht  per-day assistant turns, same order
#   hw  weekday of the last bucket, Mon=0 … Sun=6 (so the device can label
#       bars and find "this week" without a clock)
#   hm  model mix over the trailing 7 days: [[family, pct], …], top 3
#   hs  index into h of the day the rolling 7-day limit window opened
#       (from the payload's wr); absent → device uses the calendar week
DEFAULT_DAYS = 14
MIX_DAYS = 7
MIX_TOP = 3

# --- 5-hour windows ---------------------------------------------------------
# Claude's 5h limit window is first-use anchored: the first turn opens it and
# it runs exactly WINDOW_HOURS, then the next turn opens the next one. That is
# reconstructible from the transcripts (validated against a live account: the
# reconstructed open time matched the reset header's to within minutes).
#
# What is NOT in the transcripts is utilisation — the API reports a percentage
# only for the *current* window. So a window's level is either OBSERVED (the
# daemon was running and saw the peak) or ESTIMATED from output tokens via a
# ratio learned from this account's own observations. The payload distinguishes
# them: digits are estimates, letters are observed.
WINDOW_HOURS = 5
GRID_DAYS = 7
LEVEL_THRESHOLDS = (25, 50, 85)     # → levels 0..3: none / some / most / maxed
GRID_BANDS = 5                      # fixed time-of-day columns: 00-05, 05-10, 10-15, 15-20, 20-24
GRID_EMPTY = "."                    # no window opened in that band
OBSERVE_MIN_PCT = 10                # below this, tokens/pct is too noisy to fit
# The reconstructed window end and the API's reset time don't agree exactly:
# a window is anchored on the first *request*, while the transcript records the
# assistant turn, so reconstruction runs a few minutes late (measured ~5 min on
# a live account). Match observations to windows by proximity, not equality.
WINDOW_MATCH_MINUTES = 45

# Bump whenever the state file gains a field derived during ingest. The stored
# per-file offsets sit at EOF, so a new field would stay empty forever on an
# existing install — a version mismatch discards the offsets and forces one
# full re-read instead.
STATE_VERSION = 2

_FAMILIES = ("opus", "fable", "sonnet", "haiku")


def window_rank(ch: str) -> int:
    """Level 0-3 for a grid character, or -1 for an empty band."""
    if ch in "0123":
        return int(ch)
    if ch in "abcd":
        return ord(ch) - ord("a")
    return -1


def model_family(model: str | None) -> str:
    """Collapse a model id to its family name for the mix ("Opus", "Fable", …)."""
    m = (model or "").lower()
    for fam in _FAMILIES:
        if fam in m:
            return fam.capitalize()
    return "Other"


@dataclass
class Window:
    """One reconstructed 5-hour window."""
    start: datetime
    end: datetime
    out: int = 0
    turns: int = 0
    pct: int = 0            # utilisation, observed or estimated
    observed: bool = False  # True when the daemon actually saw this window's peak


@dataclass
class DayTotals:
    turns: int = 0
    out: int = 0
    inp: int = 0
    cache_read: int = 0
    cache_write: int = 0
    by_model: dict[str, int] = field(default_factory=dict)   # family → output tokens

    def add(self, usage: dict, family: str) -> None:
        self.turns += 1
        out = int(usage.get("output_tokens") or 0)
        self.out += out
        self.inp += int(usage.get("input_tokens") or 0)
        self.cache_read += int(usage.get("cache_read_input_tokens") or 0)
        self.cache_write += int(usage.get("cache_creation_input_tokens") or 0)
        self.by_model[family] = self.by_model.get(family, 0) + out

    def to_json(self) -> dict:
        return {"t": self.turns, "o": self.out, "i": self.inp,
                "cr": self.cache_read, "cw": self.cache_write, "m": self.by_model}

    @classmethod
    def from_json(cls, d: dict) -> "DayTotals":
        return cls(turns=int(d.get("t", 0)), out=int(d.get("o", 0)), inp=int(d.get("i", 0)),
                   cache_read=int(d.get("cr", 0)), cache_write=int(d.get("cw", 0)),
                   by_model={str(k): int(v) for k, v in (d.get("m") or {}).items()})


class UsageHistory:
    def __init__(self, config_dirs: Iterable[Path], days: int = DEFAULT_DAYS,
                 tz: tzinfo | None = None, state_file: Path | None = None,
                 now: Callable[[], datetime] | None = None) -> None:
        self.config_dirs = [Path(d) for d in config_dirs]
        self.window_days = int(days)
        self.tz = tz or datetime.now().astimezone().tzinfo
        self.state_file = state_file
        self._now = now or (lambda: datetime.now(timezone.utc))
        self._days: dict[str, DayTotals] = {}
        self._seen: dict[str, set[str]] = {}          # day → {"req|mid", …}
        self._files: dict[str, int] = {}               # path → consumed byte offset
        # Minute-resolution token stream, the input to window reconstruction.
        # Turns arrive per-file (unordered), so they're accumulated by epoch
        # minute and sorted on demand; minute granularity keeps the state file
        # small while preserving exact 5h boundaries.
        self._minutes: dict[int, list[int]] = {}   # epoch minute → [out, turns]
        self._observed: dict[int, int] = {}        # window key → peak pct seen
        self._ratios: list[float] = []             # output tokens per 1% of limit
        self.last_scan_bytes = 0
        self.last_scan_files = 0

    # ------------------------------------------------------------------ query
    def day(self, key: str) -> DayTotals:
        return self._days.get(key, DayTotals())

    def days(self) -> list[str]:
        return sorted(self._days)

    def file_offset(self, path: Path) -> int:
        return self._files.get(str(path), 0)

    def _today(self) -> datetime:
        return self._now().astimezone(self.tz)

    def _day_keys(self) -> list[str]:
        """The window's day keys, oldest → newest (today last)."""
        today = self._today().date()
        return [(today - _days_delta(i)).isoformat() for i in range(self.window_days - 1, -1, -1)]

    def model_mix(self) -> list[list]:
        """Output-token share per model family over the trailing MIX_DAYS."""
        totals: dict[str, int] = {}
        for key in self._day_keys()[-MIX_DAYS:]:
            for fam, out in self._days.get(key, DayTotals()).by_model.items():
                totals[fam] = totals.get(fam, 0) + out
        grand = sum(totals.values())
        if grand <= 0:
            return []
        ranked = sorted(totals.items(), key=lambda kv: (-kv[1], kv[0]))[:MIX_TOP]
        return [[fam, int(round(out * 100 / grand))] for fam, out in ranked]

    def week_start_index(self, reset_minutes) -> int | None:
        """Index into the day buckets of the day Anthropic's rolling 7-day
        window opened, given minutes until it resets (the payload's "wr").

        The window is exactly 7 days, so it opened at reset - 7d. None when
        no reset is known (enterprise accounts report 0; no-data beats carry
        nothing), which tells the device to fall back to the calendar week.
        """
        try:
            mins = int(reset_minutes or 0)
        except (TypeError, ValueError):
            return None
        if mins <= 0:
            return None
        opened = self._now() + timedelta(minutes=mins) - timedelta(days=7)
        key = opened.astimezone(self.tz).date().isoformat()
        keys = self._day_keys()
        if key in keys:
            return keys.index(key)
        return 0 if key < keys[0] else len(keys) - 1

    def payload_fields(self) -> dict:
        keys = self._day_keys()
        grid = self.grid_field()
        flat = "".join(c for c in "".join(grid) if c != GRID_EMPTY)
        return {
            "h":  [int(round(self._days.get(k, DayTotals()).out / 1000)) for k in keys],
            "ht": [self._days.get(k, DayTotals()).turns for k in keys],
            "hw": self._today().weekday(),
            "hm": self.model_mix(),
            "wg": grid,                                        # 5h windows per day
            "wn": len(flat),                                   # windows this week
            "wx": sum(1 for c in flat if c in "3d"),           # of which maxed
        }

    # ---------------------------------------------------------------- windows
    DEFAULT_TOKENS_PER_PCT = 4500.0   # bootstrap until this account is observed

    def tokens_per_pct(self) -> float:
        """Output tokens per 1% of the 5h limit, as learned from observations.

        The real limit weighs models and input/cache tokens, none of which the
        transcripts expose in the API's terms — so rather than hardcode a
        conversion, fit one from windows where we saw both the tokens and the
        reported percentage. Median, so one odd window can't drag it.
        """
        if not self._ratios:
            return self.DEFAULT_TOKENS_PER_PCT
        xs = sorted(self._ratios)
        return xs[len(xs) // 2]

    def _window_key(self, end: datetime) -> int:
        """Stable id for a window from its end time (epoch minutes / 5)."""
        return int(end.timestamp() // 300)

    def observe(self, session_pct, reset_minutes) -> None:
        """Record the live 5h utilisation against the window it belongs to.

        Called each poll with the API's numbers. Keeps the peak per window (a
        window only ever climbs, but a reset mid-poll must not overwrite it),
        and fits the tokens→percent ratio once the window is far enough along
        for the division to mean anything.
        """
        try:
            pct = int(session_pct or 0)
            mins = int(reset_minutes or 0)
        except (TypeError, ValueError):
            return
        if pct <= 0 or mins <= 0:
            return
        api_end = self._now() + timedelta(minutes=mins)
        win = self._match_window(api_end)
        if win is None:
            # The API sees usage we can't place locally yet — the transcript
            # may not be flushed, or the work happened on another machine.
            # Next poll will find it once the turn lands.
            return
        key = self._window_key(win.end)
        if pct > self._observed.get(key, 0):
            self._observed[key] = min(pct, 100)
        if pct >= OBSERVE_MIN_PCT and win.out > 0:
            ratio = win.out / float(pct)
            self._ratios = ([r for r in self._ratios if r != ratio] + [ratio])[-16:]

    def _match_window(self, api_end: datetime) -> "Window | None":
        """The reconstructed window whose end is nearest ``api_end``, within
        WINDOW_MATCH_MINUTES. None when nothing is close enough."""
        best, best_delta = None, timedelta(minutes=WINDOW_MATCH_MINUTES)
        for w in self._reconstruct():
            delta = abs(w.end - api_end)
            if delta <= best_delta:
                best, best_delta = w, delta
        return best

    def _reconstruct(self) -> list[Window]:
        """Walk the minute stream, first-use anchoring each 5h window."""
        wins: list[Window] = []
        cur: Window | None = None
        span = timedelta(hours=WINDOW_HOURS)
        for minute in sorted(self._minutes):
            at = datetime.fromtimestamp(minute * 60, timezone.utc)
            out, turns = self._minutes[minute]
            if cur is None or at >= cur.end:
                cur = Window(start=at, end=at + span)
                wins.append(cur)
            cur.out += out
            cur.turns += turns
        return wins

    def windows(self) -> list[Window]:
        """Reconstructed windows with a level on each: observed where we saw
        one, otherwise estimated from tokens."""
        ratio = self.tokens_per_pct()
        wins = self._reconstruct()
        for w in wins:
            seen = self._observed.get(self._window_key(w.end))
            if seen is not None:
                w.pct, w.observed = seen, True
            else:
                w.pct = min(100, int(round(w.out / ratio))) if ratio > 0 else 0
        return wins

    def _level_char(self, pct: int, observed: bool) -> str:
        level = sum(1 for t in LEVEL_THRESHOLDS if pct >= t)
        return "abcd"[level] if observed else "0123"[level]

    def grid_field(self, days: int = GRID_DAYS) -> list[str]:
        """One fixed-width string per local day, oldest → newest.

        Position is the *time of day* the window opened, not its ordinal —
        five 5-hour bands (00-05, 05-10, 10-15, 15-20, 20-24), so columns line
        up across days and you can read "I max out in the mornings" off the
        grid. GRID_EMPTY marks a band with no window.

        Two windows can't share a band: their starts are always at least 5 h
        apart. The max() is belt-and-braces for clock changes.
        """
        keys = self._day_keys()[-days:]
        rows = {k: [GRID_EMPTY] * GRID_BANDS for k in keys}
        for w in self.windows():
            local = w.start.astimezone(self.tz)
            key = local.date().isoformat()
            if key not in rows:
                continue
            band = min(local.hour // 5, GRID_BANDS - 1)
            ch = self._level_char(w.pct, w.observed)
            cur = rows[key][band]
            if cur == GRID_EMPTY or window_rank(ch) > window_rank(cur):
                rows[key][band] = ch
        return ["".join(rows[k]) for k in keys]

    # ------------------------------------------------------------------- scan
    def scan(self) -> None:
        """Read whatever was appended since the last scan; update the buckets."""
        self.last_scan_bytes = 0
        self.last_scan_files = 0
        # Files not touched inside the window (+1 day of slack for the local /
        # UTC boundary) can't contribute a bucket we keep, so don't even open them.
        cutoff = self._now().timestamp() - (self.window_days + 1) * 86400
        live: set[str] = set()
        for cfg in self.config_dirs:
            root = cfg / "projects"
            if not root.is_dir():
                continue
            for path in root.glob("*/*.jsonl"):
                try:
                    st = path.stat()
                except OSError:
                    continue
                if st.st_mtime < cutoff:
                    continue
                key = str(path)
                live.add(key)
                offset = self._files.get(key, 0)
                if st.st_size < offset:
                    offset = 0            # rewritten/truncated: start over (dedup absorbs replays)
                if st.st_size == offset:
                    continue
                self._files[key] = self._consume(path, offset)
                self.last_scan_files += 1
        # Forget offsets for files that aged out so the state file stays bounded.
        for key in list(self._files):
            if key not in live:
                del self._files[key]
        self._prune()

    def _consume(self, path: Path, offset: int) -> int:
        """Parse complete lines from ``offset`` to EOF. Returns the new offset.

        A trailing line without a newline is a write in progress; leave it for
        the next scan rather than parsing half a JSON object.
        """
        try:
            with path.open("rb") as fh:
                fh.seek(offset)
                data = fh.read()
        except OSError:
            return offset
        end = data.rfind(b"\n")
        if end < 0:
            return offset
        chunk = data[: end + 1]
        self.last_scan_bytes += len(chunk)
        for raw in chunk.split(b"\n"):
            if b'"usage"' not in raw:
                continue
            self._ingest_line(raw)
        return offset + len(chunk)

    def _ingest_line(self, raw: bytes) -> None:
        try:
            d = json.loads(raw)
        except (ValueError, UnicodeDecodeError):
            return
        if not isinstance(d, dict) or d.get("type") != "assistant":
            return
        msg = d.get("message") or {}
        usage = msg.get("usage")
        if not isinstance(usage, dict):
            return
        model = msg.get("model") or ""
        if model.startswith("<"):          # "<synthetic>" placeholders carry no real usage
            return
        day = _local_day(d.get("timestamp"), self.tz)
        if day is None:
            return
        minute = _epoch_minute(d.get("timestamp"))
        ident = f"{d.get('requestId') or ''}|{msg.get('id') or d.get('uuid') or ''}"
        seen = self._seen.setdefault(day, set())
        if ident in seen:
            return
        seen.add(ident)
        self._days.setdefault(day, DayTotals()).add(usage, model_family(model))
        if minute is not None:
            slot = self._minutes.setdefault(minute, [0, 0])
            slot[0] += int(usage.get("output_tokens") or 0)
            slot[1] += 1

    def _prune(self) -> None:
        keep = set(self._day_keys())
        for key in list(self._days):
            if key not in keep:
                del self._days[key]
        for key in list(self._seen):
            if key not in keep:
                del self._seen[key]
        # The minute stream and observations only feed the window grid, so they
        # retain a shorter horizon than the day chart.
        floor = int((self._now() - timedelta(days=GRID_DAYS + 1)).timestamp() // 60)
        for m in [m for m in self._minutes if m < floor]:
            del self._minutes[m]
        key_floor = self._window_key(self._now() - timedelta(days=GRID_DAYS + 1))
        for k in [k for k in self._observed if k < key_floor]:
            del self._observed[k]

    # ------------------------------------------------------------ persistence
    def load(self) -> None:
        if not self.state_file or not self.state_file.exists():
            return
        try:
            d = json.loads(self.state_file.read_text())
            if int(d.get("v", 1)) != STATE_VERSION:
                # Older schema: drop everything and re-read from scratch.
                self._files, self._days, self._seen = {}, {}, {}
                self._minutes, self._observed, self._ratios = {}, {}, []
                return
            self._files = {str(k): int(v) for k, v in (d.get("files") or {}).items()}
            self._days = {k: DayTotals.from_json(v) for k, v in (d.get("days") or {}).items()}
            self._seen = {k: set(v) for k, v in (d.get("seen") or {}).items()}
            self._minutes = {int(k): list(v) for k, v in (d.get("minutes") or {}).items()}
            self._observed = {int(k): int(v) for k, v in (d.get("observed") or {}).items()}
            self._ratios = [float(x) for x in (d.get("ratios") or [])]
        except (ValueError, TypeError, AttributeError, OSError):
            # A corrupt or foreign state file just means a fresh full scan.
            self._files, self._days, self._seen = {}, {}, {}
            self._minutes, self._observed, self._ratios = {}, {}, []

    def save(self) -> None:
        if not self.state_file:
            return
        payload = {
            "v": STATE_VERSION,
            "files": self._files,
            "days": {k: v.to_json() for k, v in self._days.items()},
            "seen": {k: sorted(v) for k, v in self._seen.items()},
            "minutes": self._minutes,
            "observed": self._observed,
            "ratios": self._ratios,
        }
        tmp = self.state_file.with_name(self.state_file.name + f".{os.getpid()}.tmp")
        try:
            self.state_file.parent.mkdir(parents=True, exist_ok=True)
            tmp.write_text(json.dumps(payload, separators=(",", ":")))
            os.replace(tmp, self.state_file)
        except OSError:
            try:
                tmp.unlink()
            except OSError:
                pass


# --------------------------------------------------------------------- helpers

def _days_delta(n: int):
    from datetime import timedelta
    return timedelta(days=n)


def _epoch_minute(ts: str | None) -> int | None:
    if not ts or not isinstance(ts, str):
        return None
    try:
        dt = datetime.fromisoformat(ts.replace("Z", "+00:00"))
    except ValueError:
        return None
    if dt.tzinfo is None:
        dt = dt.replace(tzinfo=timezone.utc)
    return int(dt.timestamp() // 60)


def _local_day(ts: str | None, tz: tzinfo) -> str | None:
    if not ts or not isinstance(ts, str):
        return None
    try:
        dt = datetime.fromisoformat(ts.replace("Z", "+00:00"))
    except ValueError:
        return None
    if dt.tzinfo is None:
        dt = dt.replace(tzinfo=timezone.utc)
    return dt.astimezone(tz).date().isoformat()


# ------------------------------------------------------------------------ CLI

def _main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Aggregate local Claude Code usage by day.")
    ap.add_argument("--config-dir", action="append", type=Path,
                    help="Claude config dir to scan (repeatable; default ~/.claude)")
    ap.add_argument("--days", type=int, default=DEFAULT_DAYS)
    ap.add_argument("--state", type=Path,
                    default=Path.home() / ".config" / "claude-usage-monitor" / "history-state.json")
    ap.add_argument("--no-state", action="store_true", help="don't read or write the state file")
    ap.add_argument("--json", action="store_true", help="print the payload fields instead of a table")
    args = ap.parse_args(argv)

    dirs = args.config_dir or [Path.home() / ".claude"]
    hist = UsageHistory(dirs, days=args.days, state_file=None if args.no_state else args.state)
    hist.load()
    t0 = time.time()
    hist.scan()
    hist.save()
    elapsed = time.time() - t0

    if args.json:
        print(json.dumps(hist.payload_fields(), separators=(",", ":")))
        return 0

    print(f"scanned {hist.last_scan_files} files, {hist.last_scan_bytes/1e6:.1f} MB new, {elapsed:.2f}s")
    print(f"{'day':10} {'turns':>6} {'out':>10} {'in':>8} {'cache_r':>12} {'cache_w':>10}  models")
    for key in hist._day_keys():
        d = hist.day(key)
        mix = ", ".join(f"{k}:{v // 1000}k" for k, v in sorted(d.by_model.items(), key=lambda kv: -kv[1]))
        print(f"{key:10} {d.turns:6d} {d.out:10d} {d.inp:8d} {d.cache_read:12d} {d.cache_write:10d}  {mix}")
    print("mix (7d):", hist.model_mix())
    p = hist.payload_fields()
    print(f"payload: {len(json.dumps(p, separators=(',', ':')))} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(_main())
