#!/usr/bin/env python3
"""Stats workspace messages built from Claude Code's stats cache.
Run: python -m pytest daemon/tests/test_stats.py -q"""
import datetime
import json

from daemon import stats as d

TODAY = datetime.date(2026, 10, 8)  # a Thursday


def cache(active_days):
    return {
        "dailyActivity": [{"date": day, "messageCount": count} for day, count in active_days.items()],
        "modelUsage": {
            "claude-opus-5": {"inputTokens": 6_000_000, "outputTokens": 140_000_000,
                              "cacheReadInputTokens": 42_000_000_000, "cacheCreationInputTokens": 0},
            "claude-haiku-4-5-20251001": {"outputTokens": 10},
        },
        "totalSessions": 584,
        "firstSessionDate": "2026-06-17T21:08:14.857Z",
    }


def unpack(heat):
    return [x for c in heat for x in divmod(ord(c) - 65, 5)]


def test_heatmap():
    hm, st = d.stats_messages(cache({"2026-10-08": 50, "2026-10-07": 5, "2026-04-12": 1}), TODAY)
    assert hm["n"] == 7 * 25 + 5                  # 25 full weeks plus Sun..Thu
    levels = unpack(hm["h"])
    assert levels[0] > 0 and levels[hm["n"] - 1] == 4 and levels[hm["n"] - 2] > 0
    assert sum(1 for v in levels if v) == 3
    assert hm["mo"][0] == ["Apr", 0] and hm["mo"][-1] == ["Oct", 25]
    for msg in (hm, st):
        assert len(json.dumps(msg, separators=(",", ":"))) <= 220


def test_numbers():
    days = {f"2026-09-{n:02d}": 3 for n in range(10, 15)}          # a 5-day run
    days.update({"2026-10-07": 2, "2026-10-06": 2, "2026-10-04": 1})
    _, st = d.stats_messages(cache(days), TODAY)
    v = dict(st["v"])
    assert v["Longest streak"] == "5 days"
    assert v["Current streak"] == "2 days"                        # today idle: counts from yesterday
    assert v["Active days"] == "8/114"
    assert v["Sessions"] == "584" and v["Total tokens"] == "42.1b"
    assert v["Favorite model"] == "Opus 5"


def test_formatting():
    assert d._pretty_model("claude-opus-5-5") == "Opus 5.5"
    assert d._pretty_model("claude-haiku-4-5-20251001") == "Haiku 4.5"
    assert d._pretty_model("gpt-5.6-sol") == "gpt-5.6-sol"
    assert (d._human(43_403_457_835), d._human(140_289_680), d._human(999)) == ("43.4b", "140.3m", "999")
