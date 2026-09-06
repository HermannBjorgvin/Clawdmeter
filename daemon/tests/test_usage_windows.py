#!/usr/bin/env python3
"""Tests for 5-hour window reconstruction and the maxing grid.

Windows are first-use anchored: the first turn opens a window that runs
exactly 5h; the next turn after it expires opens the next one. Verified
against the live API on a real account (reconstructed open time matched the
header-derived one to within minutes).

Run: python -m pytest daemon/tests/test_usage_windows.py -x -q
"""
import json
from datetime import datetime, timedelta, timezone
from pathlib import Path

import pytest

from daemon.usage_history import UsageHistory, WINDOW_HOURS

UTC = timezone.utc
NOW = datetime(2026, 9, 6, 12, 0, tzinfo=UTC)


def _turn(ts: str, out: int = 100, req: str = "r", mid: str = "m") -> str:
    return json.dumps({
        "type": "assistant", "timestamp": ts, "requestId": req,
        "message": {"id": mid, "model": "claude-opus-5",
                    "usage": {"output_tokens": out, "input_tokens": 1,
                              "cache_read_input_tokens": 0, "cache_creation_input_tokens": 0}},
    })


def _write(path: Path, *lines: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n")


@pytest.fixture
def cfg(tmp_path: Path) -> Path:
    d = tmp_path / ".claude"
    (d / "projects" / "p").mkdir(parents=True)
    return d


def _hist(cfg, tz=UTC, state=None, now=NOW, days=14):
    return UsageHistory([cfg], days=days, tz=tz, state_file=state, now=lambda: now)


# ---------------------------------------------------------------------------
# reconstruction
# ---------------------------------------------------------------------------

def test_single_burst_is_one_window(cfg):
    _write(cfg / "projects/p/s.jsonl",
           _turn("2026-09-06T01:00:00Z", 10, "r1", "m1"),
           _turn("2026-09-06T02:00:00Z", 20, "r2", "m2"),
           _turn("2026-09-06T05:30:00Z", 30, "r3", "m3"))
    h = _hist(cfg); h.scan()
    w = h.windows()
    assert len(w) == 1
    assert w[0].start == datetime(2026, 9, 6, 1, 0, tzinfo=UTC)
    assert w[0].out == 60 and w[0].turns == 3


def test_turn_after_expiry_opens_a_new_window(cfg):
    # 01:00 opens a window closing at 06:00; 06:00 is outside it.
    _write(cfg / "projects/p/s.jsonl",
           _turn("2026-09-06T01:00:00Z", 10, "r1", "m1"),
           _turn("2026-09-06T06:00:00Z", 20, "r2", "m2"))
    h = _hist(cfg); h.scan()
    w = h.windows()
    assert len(w) == 2
    assert w[1].start == datetime(2026, 9, 6, 6, 0, tzinfo=UTC)


def test_turn_just_inside_expiry_stays_in_window(cfg):
    _write(cfg / "projects/p/s.jsonl",
           _turn("2026-09-06T01:00:00Z", 10, "r1", "m1"),
           _turn("2026-09-06T05:59:00Z", 20, "r2", "m2"))
    h = _hist(cfg); h.scan()
    assert len(h.windows()) == 1


def test_windows_are_chronological_across_unordered_files(cfg):
    # Two projects scanned in arbitrary order must still yield ordered windows.
    _write(cfg / "projects/p/b.jsonl", _turn("2026-09-06T09:00:00Z", 5, "r3", "m3"))
    _write(cfg / "projects/p/a.jsonl", _turn("2026-09-06T01:00:00Z", 5, "r1", "m1"))
    h = _hist(cfg); h.scan()
    w = h.windows()
    assert [x.start.hour for x in w] == [1, 9]


def test_window_length_is_five_hours(cfg):
    _write(cfg / "projects/p/s.jsonl", _turn("2026-09-06T01:00:00Z"))
    h = _hist(cfg); h.scan()
    assert h.windows()[0].end - h.windows()[0].start == timedelta(hours=WINDOW_HOURS)


def test_minute_granularity_merges_turns_in_the_same_minute(cfg):
    _write(cfg / "projects/p/s.jsonl",
           _turn("2026-09-06T01:00:10Z", 10, "r1", "m1"),
           _turn("2026-09-06T01:00:50Z", 20, "r2", "m2"))
    h = _hist(cfg); h.scan()
    w = h.windows()
    assert len(w) == 1 and w[0].out == 30


# ---------------------------------------------------------------------------
# calibration: tokens per 1% of the 5h limit, learned from observations
# ---------------------------------------------------------------------------

def test_default_ratio_until_an_observation_lands(cfg):
    h = _hist(cfg)
    assert h.tokens_per_pct() == pytest.approx(h.DEFAULT_TOKENS_PER_PCT)


def test_observation_sets_the_ratio(cfg):
    _write(cfg / "projects/p/s.jsonl", _turn("2026-09-06T08:00:00Z", 220_000, "r1", "m1"))
    h = _hist(cfg); h.scan()
    # Live poll: the window opened at 08:00 ends at 13:00 — 60 min from NOW.
    h.observe(session_pct=55, reset_minutes=60)
    assert h.tokens_per_pct() == pytest.approx(4000, rel=0.01)   # 220000 / 55


def test_ratio_is_the_median_of_observations(cfg):
    h = _hist(cfg)
    h._ratios = [3000.0, 4000.0, 11000.0]
    assert h.tokens_per_pct() == 4000.0


def test_low_percentages_are_ignored_as_noise(cfg):
    # A window at 2% divides by almost nothing — one stray turn skews it wildly.
    _write(cfg / "projects/p/s.jsonl", _turn("2026-09-06T08:00:00Z", 5_000, "r1", "m1"))
    h = _hist(cfg); h.scan()
    h.observe(session_pct=2, reset_minutes=60)
    assert h._ratios == []


def test_observation_records_peak_not_last(cfg):
    _write(cfg / "projects/p/s.jsonl", _turn("2026-09-06T08:00:00Z", 1000, "r1", "m1"))
    h = _hist(cfg); h.scan()
    h.observe(session_pct=80, reset_minutes=60)
    h.observe(session_pct=42, reset_minutes=60)       # same window, lower reading
    key = h._window_key(datetime(2026, 9, 6, 13, 0, tzinfo=UTC))
    assert h._observed[key] == 80


def test_observed_peak_wins_over_the_estimate(cfg):
    _write(cfg / "projects/p/s.jsonl", _turn("2026-09-06T08:00:00Z", 100, "r1", "m1"))
    h = _hist(cfg); h.scan()
    h.observe(session_pct=97, reset_minutes=60)       # tiny tokens, but really maxed
    w = [x for x in h.windows() if x.observed]
    assert len(w) == 1 and w[0].pct == 97


def test_estimate_used_when_no_observation(cfg):
    _write(cfg / "projects/p/s.jsonl", _turn("2026-09-06T01:00:00Z", 200_000, "r1", "m1"))
    h = _hist(cfg)
    h._ratios = [4000.0]
    h.scan()
    w = h.windows()[0]
    assert not w.observed
    assert w.pct == 50                                # 200000 / 4000


def test_estimate_is_capped_at_100(cfg):
    _write(cfg / "projects/p/s.jsonl", _turn("2026-09-06T01:00:00Z", 10_000_000, "r1", "m1"))
    h = _hist(cfg); h._ratios = [4000.0]; h.scan()
    assert h.windows()[0].pct == 100


# ---------------------------------------------------------------------------
# payload encoding
# ---------------------------------------------------------------------------

def test_grid_payload_groups_windows_by_local_day(cfg):
    _write(cfg / "projects/p/s.jsonl",
           _turn("2026-09-05T01:00:00Z", 100, "r1", "m1"),
           _turn("2026-09-05T09:00:00Z", 100, "r2", "m2"),
           _turn("2026-09-06T01:00:00Z", 100, "r3", "m3"))
    h = _hist(cfg); h.scan()
    g = h.grid_field(days=7)
    assert len(g) == 7
    assert all(len(row) == 5 for row in g)          # fixed-width: one cell per band
    # 5th: windows at 01:00 (band 0) and 09:00 (band 1); 6th: 01:00 (band 0)
    assert [c != "." for c in g[-2]] == [True, True, False, False, False]
    assert [c != "." for c in g[-1]] == [True, False, False, False, False]


def test_grid_levels_are_digits_when_estimated(cfg):
    _write(cfg / "projects/p/s.jsonl", _turn("2026-09-06T01:00:00Z", 200_000, "r1", "m1"))
    h = _hist(cfg); h._ratios = [4000.0]; h.scan()
    assert h.grid_field(days=7)[-1] == "2...."      # 01:00 → band 0; 50% → level 2


def test_grid_levels_are_letters_when_observed(cfg):
    _write(cfg / "projects/p/s.jsonl", _turn("2026-09-06T08:00:00Z", 100, "r1", "m1"))
    h = _hist(cfg); h.scan()
    h.observe(session_pct=97, reset_minutes=60)
    assert h.grid_field(days=7)[-1] == ".d..."      # 08:00 → band 1; maxed, observed


@pytest.mark.parametrize("pct,ch", [(0, "0"), (5, "0"), (24, "0"), (25, "1"),
                                    (49, "1"), (50, "2"), (84, "2"), (85, "3"), (100, "3")])
def test_level_thresholds(cfg, pct, ch):
    h = _hist(cfg)
    assert h._level_char(pct, observed=False) == ch


def test_grid_is_empty_string_for_days_with_no_windows(cfg):
    h = _hist(cfg); h.scan()
    assert h.grid_field(days=7) == ["....."] * 7


def test_payload_includes_grid_and_maxed_count(cfg):
    _write(cfg / "projects/p/s.jsonl",
           _turn("2026-09-05T01:00:00Z", 400_000, "r1", "m1"),
           _turn("2026-09-06T01:00:00Z", 100, "r2", "m2"))
    h = _hist(cfg); h._ratios = [4000.0]; h.scan()
    p = h.payload_fields()
    assert "wg" in p and len(p["wg"]) == 7
    assert p["wg"][-2] == "3...."                    # 400k/4000 = 100% → maxed, band 0
    assert p["wn"] == 2                              # windows this week
    assert p["wx"] == 1                              # maxed


def test_grid_payload_stays_small(cfg):
    # A heavy week: 5 windows a day for 7 days.
    lines = []
    for d in range(7):
        for w in range(5):
            ts = f"2026-08-3{d} {w*5:02d}:00:00".replace(" ", "T") + "Z"
            lines.append(_turn(ts, 300_000, f"r{d}{w}", f"m{d}{w}"))
    _write(cfg / "projects/p/s.jsonl", *lines)
    h = _hist(cfg, now=datetime(2026, 9, 5, 12, 0, tzinfo=UTC)); h.scan()
    encoded = json.dumps(h.payload_fields(), separators=(",", ":")).encode()
    assert len(encoded) < 260, len(encoded)


# ---------------------------------------------------------------------------
# persistence
# ---------------------------------------------------------------------------

def test_observations_and_ratios_survive_a_restart(cfg, tmp_path):
    state = tmp_path / "s.json"
    _write(cfg / "projects/p/s.jsonl", _turn("2026-09-06T08:00:00Z", 220_000, "r1", "m1"))
    h1 = _hist(cfg, state=state); h1.scan(); h1.observe(55, 60); h1.save()

    h2 = _hist(cfg, state=state); h2.load(); h2.scan()
    assert h2.tokens_per_pct() == pytest.approx(4000, rel=0.01)
    assert [w.pct for w in h2.windows() if w.observed] == [55]


def test_observation_tolerates_a_large_reconstruction_drift(cfg):
    # Measured in the wild: the API's window ended 54 min after the
    # reconstructed one. Overlap is still ~4h, so it must match.
    _write(cfg / "projects/p/s.jsonl", _turn("2026-09-06T08:00:00Z", 100_000, "r1", "m1"))
    h = _hist(cfg); h.scan()
    h.observe(session_pct=50, reset_minutes=114)       # api window ends 13:54 vs 13:00
    assert [w.pct for w in h.windows() if w.observed] == [50]


def test_observation_rejects_a_window_overlapping_less_than_half(cfg):
    _write(cfg / "projects/p/s.jsonl", _turn("2026-09-06T08:00:00Z", 100_000, "r1", "m1"))
    h = _hist(cfg); h.scan()
    # api window [11:00, 16:00) vs local [08:00, 13:00) → 2h overlap, under 2.5h
    h.observe(session_pct=50, reset_minutes=240)
    assert not any(w.observed for w in h.windows())


def test_observation_tolerates_the_reconstruction_skew(cfg):
    # Reconstruction lags the true window start by a few minutes (the window is
    # anchored on the request, the transcript records the response), so the API
    # reset and the reconstructed end differ. A small skew must still match.
    _write(cfg / "projects/p/s.jsonl", _turn("2026-09-06T08:00:00Z", 100_000, "r1", "m1"))
    h = _hist(cfg); h.scan()
    h.observe(session_pct=50, reset_minutes=55)        # 5 min off → same window
    assert [w.pct for w in h.windows() if w.observed] == [50]


def test_observation_ignores_a_window_it_cannot_place(cfg):
    _write(cfg / "projects/p/s.jsonl", _turn("2026-09-06T08:00:00Z", 100_000, "r1", "m1"))
    h = _hist(cfg); h.scan()
    h.observe(session_pct=50, reset_minutes=240)       # 3 h off → no match
    assert not any(w.observed for w in h.windows())
    assert h._ratios == []


def test_old_state_schema_forces_a_full_rescan(cfg, tmp_path):
    """A state file from before a field existed must not leave it empty forever.

    The per-file offsets sit at EOF, so an incremental scan reads nothing and
    any newly-derived field (here: the minute stream behind the window grid)
    would stay blank until the file aged out.
    """
    import json as _json
    state = tmp_path / "s.json"
    src = cfg / "projects/p/s.jsonl"
    _write(src, _turn("2026-09-06T08:00:00Z", 100_000, "r1", "m1"))

    # A v1-era state: offsets at EOF, day totals present, no minute stream.
    state.write_text(_json.dumps({
        "files": {str(src): src.stat().st_size},
        "days": {"2026-09-06": {"t": 1, "o": 100_000, "i": 0, "cr": 0, "cw": 0, "m": {"Opus": 100_000}}},
        "seen": {"2026-09-06": ["r1|m1"]},
    }))

    h = _hist(cfg, state=state)
    h.load()
    h.scan()
    assert h.windows(), "old state must trigger a re-read so windows can be built"
    assert h.day("2026-09-06").out == 100_000     # and not double-counted


def test_current_state_schema_is_reused_incrementally(cfg, tmp_path):
    state = tmp_path / "s.json"
    _write(cfg / "projects/p/s.jsonl", _turn("2026-09-06T08:00:00Z", 100_000, "r1", "m1"))
    h1 = _hist(cfg, state=state); h1.scan(); h1.save()
    h2 = _hist(cfg, state=state); h2.load(); h2.scan()
    assert h2.last_scan_bytes == 0
    assert len(h2.windows()) == 1


# ---------------------------------------------------------------------------
# time-of-day bands
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("hour,band", [(0, 0), (4, 0), (5, 1), (9, 1), (10, 2),
                                       (14, 2), (15, 3), (19, 3), (20, 4), (23, 4)])
def test_window_lands_in_its_time_band(cfg, hour, band):
    _write(cfg / "projects/p/s.jsonl", _turn(f"2026-09-06T{hour:02d}:30:00Z", 200_000, "r1", "m1"))
    h = _hist(cfg); h._ratios = [4000.0]; h.scan()
    row = h.grid_field(days=7)[-1]
    assert row[band] != "." and row.count(".") == 4


def test_bands_use_local_time_not_utc(cfg):
    # 23:30 UTC is 09:30 next day at UTC+10 → band 1 of the *following* day.
    _write(cfg / "projects/p/s.jsonl", _turn("2026-09-05T23:30:00Z", 200_000, "r1", "m1"))
    h = UsageHistory([cfg], days=14, tz=timezone(timedelta(hours=10)), now=lambda: NOW)
    h._ratios = [4000.0]; h.scan()
    assert h.grid_field(days=7)[-1] == ".2..."


def test_columns_align_across_days(cfg):
    # Both days used the 10-15 band and nothing else; the grid must show that
    # in the same column, regardless of how many windows each day had.
    _write(cfg / "projects/p/s.jsonl",
           _turn("2026-09-05T11:00:00Z", 200_000, "r1", "m1"),
           _turn("2026-09-05T02:00:00Z", 200_000, "r2", "m2"),
           _turn("2026-09-06T12:00:00Z", 200_000, "r3", "m3"))
    h = _hist(cfg); h._ratios = [4000.0]; h.scan()
    g = h.grid_field(days=7)
    assert g[-2][2] != "." and g[-1][2] != "."      # same column on both days
    assert g[-2][0] != "." and g[-1][0] == "."      # only the 5th used band 0


def test_window_count_ignores_empty_bands(cfg):
    _write(cfg / "projects/p/s.jsonl",
           _turn("2026-09-05T11:00:00Z", 200_000, "r1", "m1"),
           _turn("2026-09-06T12:00:00Z", 200_000, "r2", "m2"))
    h = _hist(cfg); h._ratios = [4000.0]; h.scan()
    p = h.payload_fields()
    assert p["wn"] == 2


# ---------------------------------------------------------------------------
# current (still-open) window — the cell the device highlights
# ---------------------------------------------------------------------------

def test_current_cell_points_at_the_open_window(cfg):
    # NOW is Sun 12:00Z. A window opened 08:00 (band 1) closes at 13:00.
    _write(cfg / "projects/p/s.jsonl", _turn("2026-09-06T08:00:00Z", 100_000, "r1", "m1"))
    h = _hist(cfg); h.scan()
    # day index 6 (today, last of 7) * 5 bands + band 1
    assert h.current_cell(60) == 6 * 5 + 1


def test_current_cell_handles_a_window_opened_yesterday(cfg):
    # Opened 22:00 on the 5th (band 4), still open at 12:00 on the 6th... a 5h
    # window from 22:00 closes at 03:00, so use one that genuinely spans: the
    # device must not assume "current" is always in today's row.
    _write(cfg / "projects/p/s.jsonl", _turn("2026-09-05T23:00:00Z", 100_000, "r1", "m1"))
    h = UsageHistory([cfg], days=14, tz=UTC, now=lambda: datetime(2026, 9, 6, 2, 0, tzinfo=UTC))
    h.scan()
    # Window 23:00 → 04:00; at 02:00 it has 120 min left. Row is the 5th, not the 6th.
    cell = h.current_cell(120)
    assert cell is not None
    assert cell // 5 == 5           # second-to-last row (the 5th)
    assert cell % 5 == 4            # band 20-24


def test_current_cell_absent_without_a_reset(cfg):
    _write(cfg / "projects/p/s.jsonl", _turn("2026-09-06T08:00:00Z", 100_000, "r1", "m1"))
    h = _hist(cfg); h.scan()
    assert h.current_cell(0) is None
    assert h.current_cell(None) is None


def test_current_cell_absent_when_no_local_window_matches(cfg):
    # Usage on another machine: the API reports a window we can't place locally.
    _write(cfg / "projects/p/s.jsonl", _turn("2026-09-06T08:00:00Z", 100_000, "r1", "m1"))
    h = _hist(cfg); h.scan()
    assert h.current_cell(240) is None


def test_current_cell_absent_when_the_window_aged_out_of_the_grid(cfg):
    _write(cfg / "projects/p/s.jsonl", _turn("2026-08-20T08:00:00Z", 100_000, "r1", "m1"))
    h = _hist(cfg); h.scan()
    assert h.current_cell(60) is None
