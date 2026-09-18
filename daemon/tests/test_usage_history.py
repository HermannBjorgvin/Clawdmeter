#!/usr/bin/env python3
"""Unit tests for daemon/usage_history.py — local-transcript usage aggregation.

The aggregator tails ``<config_dir>/projects/*/*.jsonl``, buckets assistant
turns by *local* day, dedups by (requestId, message.id), and emits compact
payload fields for the device's History screen.

Run: python -m pytest daemon/tests/test_usage_history.py -x -q
"""
import json
from datetime import datetime, timedelta, timezone
from pathlib import Path

import pytest

from daemon.usage_history import UsageHistory, model_family

UTC = timezone.utc


# ---------------------------------------------------------------------------
# fixtures
# ---------------------------------------------------------------------------

def _turn(ts: str, out: int = 100, inp: int = 5, cr: int = 1000, cw: int = 50,
          model: str = "claude-opus-5", req: str = "req-1", mid: str = "msg-1",
          kind: str = "assistant", sidechain: bool = False) -> str:
    return json.dumps({
        "type": kind,
        "timestamp": ts,
        "requestId": req,
        "isSidechain": sidechain,
        "uuid": f"u-{req}-{mid}",
        "message": {
            "id": mid,
            "model": model,
            "usage": {
                "input_tokens": inp,
                "output_tokens": out,
                "cache_read_input_tokens": cr,
                "cache_creation_input_tokens": cw,
            },
        },
    })


def _write(path: Path, *lines: str, newline_at_end: bool = True) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    text = "\n".join(lines)
    if newline_at_end:
        text += "\n"
    path.write_text(text)


def _append(path: Path, *lines: str) -> None:
    with path.open("a") as fh:
        for line in lines:
            fh.write(line + "\n")


@pytest.fixture
def cfg(tmp_path: Path) -> Path:
    d = tmp_path / ".claude"
    (d / "projects" / "proj-a").mkdir(parents=True)
    return d


NOW = datetime(2026, 9, 6, 12, 0, tzinfo=UTC)   # a Sunday


def _hist(cfg: Path, days: int = 14, tz=UTC, state: Path | None = None,
          now: datetime = NOW) -> UsageHistory:
    return UsageHistory([cfg], days=days, tz=tz, state_file=state, now=lambda: now)


# ---------------------------------------------------------------------------
# bucketing
# ---------------------------------------------------------------------------

def test_buckets_assistant_turns_by_day(cfg):
    f = cfg / "projects" / "proj-a" / "s1.jsonl"
    _write(f,
           _turn("2026-09-05T10:00:00.000Z", out=100, req="r1", mid="m1"),
           _turn("2026-09-05T11:00:00.000Z", out=200, req="r2", mid="m2"),
           _turn("2026-09-06T01:00:00.000Z", out=300, req="r3", mid="m3"))
    h = _hist(cfg)
    h.scan()
    assert h.day("2026-09-05").out == 300
    assert h.day("2026-09-05").turns == 2
    assert h.day("2026-09-06").out == 300
    assert h.day("2026-09-06").turns == 1


def test_buckets_by_local_day_not_utc(cfg):
    # 23:30 UTC on the 5th is 09:30 on the 6th at UTC+10.
    f = cfg / "projects" / "proj-a" / "s1.jsonl"
    _write(f, _turn("2026-09-05T23:30:00.000Z", out=50))
    h = _hist(cfg, tz=timezone(timedelta(hours=10)))
    h.scan()
    assert h.day("2026-09-06").out == 50
    assert h.day("2026-09-05").turns == 0


def test_counts_all_usage_fields(cfg):
    f = cfg / "projects" / "proj-a" / "s1.jsonl"
    _write(f, _turn("2026-09-06T01:00:00.000Z", out=7, inp=3, cr=900, cw=40))
    h = _hist(cfg)
    h.scan()
    d = h.day("2026-09-06")
    assert (d.out, d.inp, d.cache_read, d.cache_write) == (7, 3, 900, 40)


def test_dedups_repeated_message_id(cfg):
    # Claude Code writes one line per content block; each repeats the same
    # requestId + message.id + usage. Count the turn once.
    f = cfg / "projects" / "proj-a" / "s1.jsonl"
    _write(f,
           _turn("2026-09-06T01:00:00.000Z", out=100, req="r1", mid="m1"),
           _turn("2026-09-06T01:00:01.000Z", out=100, req="r1", mid="m1"),
           _turn("2026-09-06T01:00:02.000Z", out=100, req="r1", mid="m1"))
    h = _hist(cfg)
    h.scan()
    assert h.day("2026-09-06").turns == 1
    assert h.day("2026-09-06").out == 100


def test_ignores_non_assistant_and_synthetic(cfg):
    f = cfg / "projects" / "proj-a" / "s1.jsonl"
    _write(f,
           _turn("2026-09-06T01:00:00.000Z", out=100, kind="user", req="r1", mid="m1"),
           _turn("2026-09-06T01:00:00.000Z", out=100, model="<synthetic>", req="r2", mid="m2"),
           json.dumps({"type": "summary", "summary": "x"}),
           "this is not json",
           _turn("2026-09-06T01:00:00.000Z", out=5, req="r3", mid="m3"))
    h = _hist(cfg)
    h.scan()
    assert h.day("2026-09-06").turns == 1
    assert h.day("2026-09-06").out == 5


def test_sidechain_turns_are_counted(cfg):
    # Subagent turns burn real tokens; they count.
    f = cfg / "projects" / "proj-a" / "agent-1.jsonl"
    _write(f, _turn("2026-09-06T01:00:00.000Z", out=42, sidechain=True))
    h = _hist(cfg)
    h.scan()
    assert h.day("2026-09-06").out == 42


def test_scans_every_configured_dir(tmp_path):
    a = tmp_path / ".claude"
    b = tmp_path / ".claude-work"
    _write(a / "projects" / "p" / "s.jsonl", _turn("2026-09-06T01:00:00.000Z", out=1, req="r1", mid="m1"))
    _write(b / "projects" / "p" / "s.jsonl", _turn("2026-09-06T01:00:00.000Z", out=2, req="r2", mid="m2"))
    h = UsageHistory([a, b], days=14, tz=UTC, now=lambda: NOW)
    h.scan()
    assert h.day("2026-09-06").out == 3


def test_missing_projects_dir_is_not_an_error(tmp_path):
    h = UsageHistory([tmp_path / "nope"], days=14, tz=UTC, now=lambda: NOW)
    h.scan()
    assert h.day("2026-09-06").turns == 0


# ---------------------------------------------------------------------------
# incremental scanning
# ---------------------------------------------------------------------------

def test_second_scan_reads_only_appended_lines(cfg):
    f = cfg / "projects" / "proj-a" / "s1.jsonl"
    _write(f, _turn("2026-09-06T01:00:00.000Z", out=10, req="r1", mid="m1"))
    h = _hist(cfg)
    h.scan()
    first_offset = h.file_offset(f)
    assert first_offset == f.stat().st_size

    _append(f, _turn("2026-09-06T02:00:00.000Z", out=20, req="r2", mid="m2"))
    h.scan()
    assert h.day("2026-09-06").out == 30
    assert h.file_offset(f) == f.stat().st_size
    assert h.last_scan_bytes == f.stat().st_size - first_offset


def test_unchanged_files_are_not_reread(cfg):
    f = cfg / "projects" / "proj-a" / "s1.jsonl"
    _write(f, _turn("2026-09-06T01:00:00.000Z", out=10))
    h = _hist(cfg)
    h.scan()
    h.scan()
    assert h.last_scan_bytes == 0
    assert h.day("2026-09-06").out == 10


def test_partial_trailing_line_is_deferred_until_complete(cfg):
    f = cfg / "projects" / "proj-a" / "s1.jsonl"
    full = _turn("2026-09-06T01:00:00.000Z", out=10, req="r1", mid="m1")
    partial = _turn("2026-09-06T02:00:00.000Z", out=20, req="r2", mid="m2")
    _write(f, full)
    with f.open("a") as fh:
        fh.write(partial[:40])           # mid-write: no newline yet
    h = _hist(cfg)
    h.scan()
    assert h.day("2026-09-06").out == 10
    assert h.file_offset(f) == len(full) + 1

    with f.open("a") as fh:
        fh.write(partial[40:] + "\n")    # writer finishes the line
    h.scan()
    assert h.day("2026-09-06").out == 30


def test_truncated_file_is_rescanned_without_double_counting(cfg):
    f = cfg / "projects" / "proj-a" / "s1.jsonl"
    line = _turn("2026-09-06T01:00:00.000Z", out=10, req="r1", mid="m1")
    _write(f, line, line + "", )   # two copies of the same turn → dedup'd to one
    h = _hist(cfg)
    h.scan()
    _write(f, line)                # file shrinks (rewritten)
    h.scan()
    assert h.day("2026-09-06").out == 10
    assert h.day("2026-09-06").turns == 1


def test_files_older_than_window_are_skipped(cfg, monkeypatch):
    import os
    f = cfg / "projects" / "proj-a" / "old.jsonl"
    _write(f, _turn("2026-09-06T01:00:00.000Z", out=999))
    old = (NOW - timedelta(days=30)).timestamp()
    os.utime(f, (old, old))
    h = _hist(cfg)
    h.scan()
    assert h.day("2026-09-06").out == 0
    assert h.last_scan_bytes == 0


# ---------------------------------------------------------------------------
# model families
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("model,family", [
    ("claude-opus-5", "Opus"),
    ("claude-opus-4-8", "Opus"),
    ("claude-fable-5", "Fable"),
    ("claude-fable-5-1", "Fable"),
    ("claude-sonnet-5", "Sonnet"),
    ("claude-haiku-4-5-20251001", "Haiku"),
    ("something-new", "Other"),
    ("", "Other"),
    (None, "Other"),
])
def test_model_family(model, family):
    assert model_family(model) == family


def test_model_mix_is_output_token_share_over_trailing_week(cfg):
    f = cfg / "projects" / "proj-a" / "s1.jsonl"
    _write(f,
           _turn("2026-09-06T01:00:00.000Z", out=600, model="claude-opus-5",   req="r1", mid="m1"),
           _turn("2026-09-05T01:00:00.000Z", out=300, model="claude-fable-5",  req="r2", mid="m2"),
           _turn("2026-09-04T01:00:00.000Z", out=100, model="claude-sonnet-5", req="r3", mid="m3"),
           # 8 days back: outside the trailing 7 → excluded from the mix
           _turn("2026-08-29T01:00:00.000Z", out=9000, model="claude-haiku-4-5", req="r4", mid="m4"))
    h = _hist(cfg)
    h.scan()
    assert h.model_mix() == [["Opus", 60], ["Fable", 30], ["Sonnet", 10]]


def test_model_mix_caps_at_three_families(cfg):
    f = cfg / "projects" / "proj-a" / "s1.jsonl"
    lines = [_turn("2026-09-06T01:00:00.000Z", out=o, model=m, req=f"r{i}", mid=f"m{i}")
             for i, (m, o) in enumerate([("claude-opus-5", 50), ("claude-fable-5", 30),
                                         ("claude-sonnet-5", 15), ("claude-haiku-4-5", 5)])]
    _write(f, *lines)
    h = _hist(cfg)
    h.scan()
    mix = h.model_mix()
    assert [m for m, _ in mix] == ["Opus", "Fable", "Sonnet"]


# ---------------------------------------------------------------------------
# payload fields
# ---------------------------------------------------------------------------

def test_payload_fields_shape(cfg):
    f = cfg / "projects" / "proj-a" / "s1.jsonl"
    _write(f,
           _turn("2026-09-06T01:00:00.000Z", out=1_224_838, req="r1", mid="m1"),
           _turn("2026-09-05T01:00:00.000Z", out=881_358,   req="r2", mid="m2"),
           _turn("2026-09-05T02:00:00.000Z", out=1,          req="r3", mid="m3"))
    h = _hist(cfg, days=14)
    h.scan()
    p = h.payload_fields()

    assert set(p) == {"h", "ht", "hw", "hm", "wg", "wn", "wx"}
    assert len(p["h"]) == 14 and len(p["ht"]) == 14
    # oldest → newest, today last; output tokens in thousands, rounded
    assert p["h"][-1] == 1225
    assert p["h"][-2] == 881
    assert p["ht"][-1] == 1 and p["ht"][-2] == 2
    assert all(isinstance(v, int) for v in p["h"] + p["ht"])
    assert p["h"][0] == 0 and p["ht"][0] == 0
    assert p["hw"] == 6                     # 2026-09-06 is a Sunday (Mon=0)
    assert p["hm"] == [["Opus", 100]]


def test_payload_fits_the_ble_budget(cfg):
    # Worst realistic case: 14 busy days, 3 families, plus a full window grid.
    # The device's RX buffer is 512 bytes and the base payload is ~105, so the
    # history block has to leave comfortable headroom.
    f = cfg / "projects" / "proj-a" / "s1.jsonl"
    lines = []
    for i in range(14):
        day = (NOW - timedelta(days=i)).strftime("%Y-%m-%dT10:00:00.000Z")
        for j, m in enumerate(["claude-opus-5", "claude-fable-5", "claude-sonnet-5"]):
            for k in range(3):
                lines.append(_turn(day, out=999_999, model=m, req=f"r{i}{j}{k}", mid=f"m{i}{j}{k}"))
    _write(f, *lines)
    h = _hist(cfg, days=14)
    h.scan()
    encoded = json.dumps(h.payload_fields(), separators=(",", ":")).encode()
    assert len(encoded) < 330, len(encoded)


def test_empty_history_still_yields_full_length_arrays(cfg):
    h = _hist(cfg, days=14)
    h.scan()
    p = h.payload_fields()
    assert p["h"] == [0] * 14
    assert p["ht"] == [0] * 14
    assert p["hm"] == []


# ---------------------------------------------------------------------------
# retention + persistence
# ---------------------------------------------------------------------------

def test_days_outside_window_are_pruned(cfg):
    f = cfg / "projects" / "proj-a" / "s1.jsonl"
    _write(f,
           _turn("2026-08-01T01:00:00.000Z", out=1, req="r1", mid="m1"),
           _turn("2026-09-06T01:00:00.000Z", out=2, req="r2", mid="m2"))
    h = _hist(cfg, days=14)
    h.scan()
    assert "2026-08-01" not in h.days()
    assert "2026-09-06" in h.days()


def test_state_roundtrip(cfg, tmp_path):
    state = tmp_path / "history-state.json"
    f = cfg / "projects" / "proj-a" / "s1.jsonl"
    _write(f, _turn("2026-09-06T01:00:00.000Z", out=10, req="r1", mid="m1"))
    h1 = _hist(cfg, state=state)
    h1.scan()
    h1.save()
    assert state.exists()

    h2 = _hist(cfg, state=state)
    h2.load()
    h2.scan()
    assert h2.last_scan_bytes == 0             # nothing new to read
    assert h2.payload_fields() == h1.payload_fields()

    # …and the persisted dedup set still protects against replays.
    _append(f, _turn("2026-09-06T01:00:00.000Z", out=10, req="r1", mid="m1"))
    h2.scan()
    assert h2.day("2026-09-06").out == 10


def test_corrupt_state_file_is_ignored(cfg, tmp_path):
    state = tmp_path / "history-state.json"
    state.write_text("{not json")
    h = _hist(cfg, state=state)
    h.load()                                    # must not raise
    h.scan()
    assert h.payload_fields()["h"] == [0] * 14


def test_save_is_atomic(cfg, tmp_path):
    state = tmp_path / "history-state.json"
    h = _hist(cfg, state=state)
    h.scan()
    h.save()
    assert not list(tmp_path.glob("history-state.json.*"))   # no temp left behind
    json.loads(state.read_text())


# ---------------------------------------------------------------------------
# rolling weekly window (Anthropic's 7-day limit, not the calendar week)
# ---------------------------------------------------------------------------

def test_week_start_index_from_reset_minutes(cfg):
    # NOW is Sunday 12:00 UTC. Resets in 4285 min (~2d 23h) → the window
    # opened 7d before that: ~4d 0.6h ago → Wednesday → index 13-4 = 9.
    h = _hist(cfg, days=14)
    assert h.week_start_index(4285) == 9


def test_week_start_index_today_when_window_just_opened(cfg):
    h = _hist(cfg, days=14)
    assert h.week_start_index(7 * 1440 - 30) == 13     # opened 30 min ago → today


def test_week_start_index_clamps_to_window(cfg):
    h = _hist(cfg, days=3)
    assert h.week_start_index(60) == 0                 # opened ~7d ago, only 3 buckets kept


def test_week_start_index_respects_local_day_boundary(cfg):
    # 23:30 UTC Saturday is already Sunday at UTC+10. Window opened 4d ago →
    # local Wednesday → index 9 (not 8, which UTC bucketing would give).
    h = UsageHistory([cfg], days=14, tz=timezone(timedelta(hours=10)), now=lambda: NOW)
    assert h.week_start_index(4285) == 9


@pytest.mark.parametrize("bad", [0, -5, None])
def test_week_start_index_absent_without_a_reset(cfg, bad):
    h = _hist(cfg, days=14)
    assert h.week_start_index(bad) is None


def test_full_payload_fits_the_device_rx_buffer(cfg):
    """End-to-end size guard: a real base payload plus the worst-case history
    block must stay well inside the firmware's 512-byte RX buffer."""
    lines = []
    for i in range(14):
        day = (NOW - timedelta(days=i)).strftime("%Y-%m-%d")
        for hour in (0, 5, 10, 15, 20):          # five 5h windows a day
            for j, m in enumerate(["claude-opus-5", "claude-fable-5", "claude-sonnet-5"]):
                lines.append(_turn(f"{day}T{hour:02d}:30:00.000Z", out=999_999, model=m,
                                   req=f"r{i}{hour}{j}", mid=f"m{i}{hour}{j}"))
    _write(cfg / "projects" / "proj-a" / "s1.jsonl", *lines)
    h = _hist(cfg, days=14)
    h.scan()
    base = {"s": 100, "sr": 299, "w": 100, "wr": 9999, "st": "allowed",
            "acct": "pro", "ok": True, "hs": 13, "t": 1785474000, "tf": 24, "c": 1}
    base.update(h.payload_fields())
    encoded = json.dumps(base, separators=(",", ":")).encode()
    assert len(encoded) < 460, len(encoded)      # 512 buffer, minus a NUL and slack
