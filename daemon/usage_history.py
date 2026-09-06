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

_FAMILIES = ("opus", "fable", "sonnet", "haiku")


def model_family(model: str | None) -> str:
    """Collapse a model id to its family name for the mix ("Opus", "Fable", …)."""
    m = (model or "").lower()
    for fam in _FAMILIES:
        if fam in m:
            return fam.capitalize()
    return "Other"


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
        return {
            "h":  [int(round(self._days.get(k, DayTotals()).out / 1000)) for k in keys],
            "ht": [self._days.get(k, DayTotals()).turns for k in keys],
            "hw": self._today().weekday(),
            "hm": self.model_mix(),
        }

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
        ident = f"{d.get('requestId') or ''}|{msg.get('id') or d.get('uuid') or ''}"
        seen = self._seen.setdefault(day, set())
        if ident in seen:
            return
        seen.add(ident)
        self._days.setdefault(day, DayTotals()).add(usage, model_family(model))

    def _prune(self) -> None:
        keep = set(self._day_keys())
        for key in list(self._days):
            if key not in keep:
                del self._days[key]
        for key in list(self._seen):
            if key not in keep:
                del self._seen[key]

    # ------------------------------------------------------------ persistence
    def load(self) -> None:
        if not self.state_file or not self.state_file.exists():
            return
        try:
            d = json.loads(self.state_file.read_text())
            self._files = {str(k): int(v) for k, v in (d.get("files") or {}).items()}
            self._days = {k: DayTotals.from_json(v) for k, v in (d.get("days") or {}).items()}
            self._seen = {k: set(v) for k, v in (d.get("seen") or {}).items()}
        except (ValueError, TypeError, AttributeError, OSError):
            # A corrupt or foreign state file just means a fresh full scan.
            self._files, self._days, self._seen = {}, {}, {}

    def save(self) -> None:
        if not self.state_file:
            return
        payload = {
            "files": self._files,
            "days": {k: v.to_json() for k, v in self._days.items()},
            "seen": {k: sorted(v) for k, v in self._seen.items()},
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
