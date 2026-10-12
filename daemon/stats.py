#!/usr/bin/env python3
"""Claude Code's /stats numbers for the device's stats workspace.

No third-party imports: the macOS daemon imports it, and the Linux daemon runs
it as a script that prints one message per line.
"""
import bisect
import datetime
import json
from pathlib import Path

# The cache behind /stats, in the default config dir.
STATS_CACHE = Path.home() / ".claude" / "stats-cache.json"
STATS_WEEKS = 26  # heatmap columns; firmware's STATS_WEEKS must match
MONTHS = "Jan Feb Mar Apr May Jun Jul Aug Sep Oct Nov Dec".split()


def _pretty_model(model: str) -> str:
    """claude-opus-5-5 -> Opus 5.5, claude-haiku-4-5-20251001 -> Haiku 4.5."""
    if not model.startswith("claude-"):
        return model
    family, *rest = model.removeprefix("claude-").split("-")
    version = ".".join(p for p in rest if p.isdigit() and len(p) < 8)
    return f"{family.capitalize()} {version}".strip()


def _human(n: float) -> str:
    """Token counts the way /stats prints them: 43.4b, 140.3m, 12.0k."""
    for div, unit in ((1e9, "b"), (1e6, "m"), (1e3, "k")):
        if n >= div:
            return f"{n / div:.1f}{unit}"
    return str(int(n))


def _days(n: int) -> str:
    return f"{n} day" if n == 1 else f"{n} days"


def stats_messages(cache: dict, today: datetime.date) -> list[dict]:
    """The two stats workspace messages built from Claude Code's stats cache.

    "hm" is a STATS_WEEKS heatmap of messages per day, week columns from the
    oldest Sunday, levels 0..4 (quarters of the active days) packed two days
    per char, with month labels by column. "st" holds six preformatted
    numbers.
    """
    counts = {a["date"]: a.get("messageCount", 0) for a in cache.get("dailyActivity", [])}
    since_sunday = (today.weekday() + 1) % 7
    start = today - datetime.timedelta(days=since_sunday + 7 * (STATS_WEEKS - 1))
    n = 7 * (STATS_WEEKS - 1) + since_sunday + 1
    window = [counts.get((start + datetime.timedelta(days=i)).isoformat(), 0) for i in range(n)]
    ranked = sorted(v for v in window if v)
    # Level by rank among active days, so the busiest quarter always reads 4.
    levels = [(4 * bisect.bisect_right(ranked, v) + len(ranked) - 1) // len(ranked) if v else 0
              for v in window]
    levels += [0] * (len(levels) % 2)
    heat = "".join(chr(65 + a * 5 + b) for a, b in zip(levels[::2], levels[1::2]))

    months, last = [], None
    for col in range(STATS_WEEKS):
        sunday = start + datetime.timedelta(days=7 * col)
        if sunday.month != last:
            months.append([MONTHS[sunday.month - 1], col])
            last = sunday.month
    if len(months) > 1 and months[1][1] < 3:
        months.pop(0)  # only a sliver of the oldest month: its label would overlap

    active = sorted(datetime.date.fromisoformat(d) for d, v in counts.items() if v)
    longest = run = 0
    for prev, day in zip([None] + active, active):
        run = run + 1 if prev and (day - prev).days == 1 else 1
        longest = max(longest, run)
    active_set = set(active)
    day = today if today in active_set else today - datetime.timedelta(days=1)
    current = 0
    while day in active_set:
        current += 1
        day -= datetime.timedelta(days=1)

    usage = cache.get("modelUsage", {})
    def tokens(u: dict) -> int:
        return sum(u.get(k, 0) for k in ("inputTokens", "outputTokens",
                                         "cacheReadInputTokens", "cacheCreationInputTokens"))
    favorite = max(usage, key=lambda m: tokens(usage[m]), default="")
    first = datetime.date.fromisoformat(cache.get("firstSessionDate", today.isoformat())[:10])
    values = [
        ["Sessions", f"{cache.get('totalSessions', 0):,}"],
        ["Total tokens", _human(sum(tokens(u) for u in usage.values()))],
        ["Active days", f"{len(active)}/{(today - first).days + 1}"],
        ["Longest streak", _days(longest)],
        ["Favorite model", _pretty_model(favorite)[:15]],
        ["Current streak", _days(current)],
    ]
    return [{"k": "hm", "n": n, "h": heat, "mo": months}, {"k": "st", "v": values}]


def load_messages(path: Path = STATS_CACHE) -> list[dict]:
    """Today's messages, or none when the cache is missing or unreadable."""
    try:
        return stats_messages(json.loads(path.read_text()), datetime.date.today())
    except (OSError, ValueError, TypeError, KeyError, AttributeError):
        return []


if __name__ == "__main__":
    for msg in load_messages():
        print(json.dumps(msg, separators=(",", ":")))
