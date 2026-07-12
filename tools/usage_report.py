#!/usr/bin/env python3
"""Build a self-contained HTML usage report from the daemon's usage_log.jsonl.

When `usage_log = on` in the config, the daemons append one JSON-lines record
per poll (~60s) with the session/weekly rate-limit utilization. This reads that
log and renders a dark, on-brand HTML page with inline SVG charts — no external
dependencies (stdlib only), so it runs in any Python 3.9+.

    python tools/usage_report.py [path/to/usage_log.jsonl] [-o report.html]

With no path it defaults to the per-OS Clawdmeter dir (same place the daemon
writes it). Writes report.html next to the log unless -o is given.
"""
import argparse
import datetime as dt
import html
import json
import os
import statistics
from pathlib import Path

ACCENT = "#d97757"
AMBER = "#d9a55a"
DIM = "#8a827a"
BG = "#191917"
PANEL = "#242220"
TEXT = "#ece7e1"


def default_log_path() -> Path:
    # Windows daemon → %LOCALAPPDATA%\Clawdmeter; macOS/Linux → ~/.config/claude-usage-monitor.
    base = os.environ.get("LOCALAPPDATA")
    if base and os.name == "nt":
        return Path(base) / "Clawdmeter" / "usage_log.jsonl"
    return Path.home() / ".config" / "claude-usage-monitor" / "usage_log.jsonl"


def load_records(path: Path) -> list[dict]:
    recs = []
    with path.open(encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                r = json.loads(line)
            except json.JSONDecodeError:
                continue
            if "ts" in r:
                recs.append(r)
    recs.sort(key=lambda r: r["ts"])
    return recs


def line_chart(series, color, w=680, h=180, ymax=100.0, label="") -> str:
    """series: list of (epoch, value). Returns an inline SVG line chart."""
    if len(series) < 2:
        return f'<div class="empty">Not enough data for "{html.escape(label)}"</div>'
    t0, t1 = series[0][0], series[-1][0]
    span = max(t1 - t0, 1)
    pad = 8
    pts = []
    for t, v in series:
        x = pad + (t - t0) / span * (w - 2 * pad)
        y = pad + (1 - min(v, ymax) / ymax) * (h - 2 * pad)
        pts.append(f"{x:.1f},{y:.1f}")
    grid = ""
    for frac in (0.25, 0.5, 0.75):
        gy = pad + frac * (h - 2 * pad)
        grid += f'<line x1="{pad}" y1="{gy:.0f}" x2="{w-pad}" y2="{gy:.0f}" stroke="{DIM}" stroke-opacity="0.18"/>'
    return (
        f'<svg viewBox="0 0 {w} {h}" class="chart" xmlns="http://www.w3.org/2000/svg">'
        f'{grid}<polyline fill="none" stroke="{color}" stroke-width="2" '
        f'stroke-linejoin="round" points="{" ".join(pts)}"/></svg>'
    )


def hour_heatmap(recs) -> str:
    """Avg session % per weekday x hour — the 'peaks & times' view."""
    days = ["Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"]
    buckets = {}
    for r in recs:
        if "s" not in r:
            continue
        d = dt.datetime.fromtimestamp(r["ts"])
        buckets.setdefault((d.weekday(), d.hour), []).append(float(r["s"]))
    if not buckets:
        return '<div class="empty">No time-of-day data yet</div>'
    cell, gap, left, top = 24, 3, 40, 18
    w = left + 24 * (cell + gap)
    h = top + 7 * (cell + gap)
    svg = [f'<svg viewBox="0 0 {w} {h}" class="heat" xmlns="http://www.w3.org/2000/svg">']
    for hh in range(0, 24, 3):
        svg.append(f'<text x="{left + hh*(cell+gap)}" y="12" fill="{DIM}" font-size="10">{hh}h</text>')
    for wd in range(7):
        y = top + wd * (cell + gap)
        svg.append(f'<text x="0" y="{y+cell*0.7:.0f}" fill="{DIM}" font-size="10">{days[wd]}</text>')
        for hh in range(24):
            x = left + hh * (cell + gap)
            vals = buckets.get((wd, hh))
            if vals:
                avg = statistics.mean(vals)
                op = 0.12 + min(avg, 100) / 100 * 0.88
                svg.append(f'<rect x="{x}" y="{y}" width="{cell}" height="{cell}" rx="4" '
                           f'fill="{ACCENT}" fill-opacity="{op:.2f}"><title>{days[wd]} {hh}h · {avg:.0f}%</title></rect>')
            else:
                svg.append(f'<rect x="{x}" y="{y}" width="{cell}" height="{cell}" rx="4" fill="{PANEL}"/>')
    svg.append("</svg>")
    return "".join(svg)


def stat_card(label: str, value: str, color: str = TEXT) -> str:
    return (f'<div class="card"><div class="cval" style="color:{color}">{html.escape(value)}</div>'
            f'<div class="clbl">{html.escape(label)}</div></div>')


def build_html(recs) -> str:
    if recs:
        t0 = dt.datetime.fromtimestamp(recs[0]["ts"])
        t1 = dt.datetime.fromtimestamp(recs[-1]["ts"])
        span_txt = f"{t0:%b %d %H:%M} — {t1:%b %d %H:%M}"
        days = max(1, (t1.date() - t0.date()).days + 1)
    else:
        span_txt, days = "no data", 0

    s_vals = [r["s"] for r in recs if "s" in r]
    w_vals = [r["w"] for r in recs if "w" in r]
    cards = [
        stat_card("Records", str(len(recs))),
        stat_card("Period", f"{days} day(s)"),
        stat_card("Avg session", f"{statistics.mean(s_vals):.0f}%" if s_vals else "—", ACCENT),
        stat_card("Peak session", f"{max(s_vals):.0f}%" if s_vals else "—", ACCENT),
        stat_card("Latest weekly", f"{w_vals[-1]:.0f}%" if w_vals else "—", AMBER),
    ]
    s_series = [(r["ts"], float(r["s"])) for r in recs if "s" in r]
    w_series = [(r["ts"], float(r["w"])) for r in recs if "w" in r]
    gen = dt.datetime.now().strftime("%Y-%m-%d %H:%M")
    return f"""<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Clawdmeter · Usage report</title>
<style>
:root{{color-scheme:dark}}
body{{margin:0;background:{BG};color:{TEXT};font-family:system-ui,-apple-system,Segoe UI,sans-serif;padding:28px;max-width:760px;margin:0 auto}}
h1{{font-size:24px;margin:0 0 2px}} h2{{font-size:16px;margin:30px 0 10px;color:{DIM};font-weight:600}}
.sub{{color:{DIM};font-size:13px;margin-bottom:22px}}
.cards{{display:grid;grid-template-columns:repeat(auto-fit,minmax(110px,1fr));gap:10px}}
.card{{background:{PANEL};border-radius:12px;padding:14px}}
.cval{{font-size:22px;font-weight:700}} .clbl{{color:{DIM};font-size:12px;margin-top:2px}}
.panel{{background:{PANEL};border-radius:12px;padding:14px;overflow-x:auto}}
.chart,.heat{{width:100%;height:auto;display:block}}
.empty{{color:{DIM};font-size:13px;padding:20px;text-align:center}}
.foot{{color:{DIM};font-size:11px;margin-top:28px}}
</style></head><body>
<h1>Usage report · Clawdmeter</h1>
<div class="sub">{html.escape(span_txt)}</div>
<div class="cards">{''.join(cards)}</div>
<h2>Session limit (5h) over time</h2>
<div class="panel">{line_chart(s_series, ACCENT, label="Session %")}</div>
<h2>Weekly limit (7d) over time</h2>
<div class="panel">{line_chart(w_series, AMBER, label="Weekly %")}</div>
<h2>When you use it most (avg session % · day × hour)</h2>
<div class="panel">{hour_heatmap(recs)}</div>
<div class="foot">Generated {gen} · source: usage_log.jsonl</div>
</body></html>"""


def main() -> int:
    ap = argparse.ArgumentParser(description="Clawdmeter usage report")
    ap.add_argument("log", nargs="?", type=Path, default=None,
                    help="path to usage_log.jsonl (default: the Clawdmeter dir)")
    ap.add_argument("-o", "--out", type=Path, default=None,
                    help="output HTML file (default: report.html next to the log)")
    args = ap.parse_args()

    log_path = args.log or default_log_path()
    if not log_path.exists():
        print(f"log not found: {log_path}")
        print("Set `usage_log = on` in the config and let the daemon run a few polls.")
        return 1
    recs = load_records(log_path)
    if not recs:
        print(f"log empty or has no valid records: {log_path}")
        return 1
    out = args.out or log_path.with_name("report.html")
    out.write_text(build_html(recs), encoding="utf-8")
    print(f"report: {out}  ({len(recs)} records)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
