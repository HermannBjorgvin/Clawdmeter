from __future__ import annotations

import time
from pathlib import Path

from daemon.dashboard_collectors import (
    collect_claude_activity,
    collect_codex_activity,
    collect_codex_usage,
)


def build_dashboard_payload(
    claude_payload: dict | None,
    profile_dir: Path,
    now: float | None = None,
) -> dict:
    scan_time = time.time() if now is None else now
    payload = dict(claude_payload or {})
    payload["v"] = 2
    payload["ts"] = int(scan_time)

    codex = collect_codex_usage(profile_dir / ".codex", now=scan_time)
    if codex:
        payload["x"] = {
            "l": [
                {"p": item["percent"], "wm": item["window_minutes"], "rm": item["reset_minutes"]}
                for item in codex.get("limits", [])[:2]
            ],
            "td": int(codex.get("tokens_today", 0)),
        }
        if plan := codex.get("plan"):
            payload["x"]["pl"] = plan

    claude_activity = collect_claude_activity(profile_dir / ".claude")
    codex_activity = collect_codex_activity(profile_dir / ".codex")
    if claude_activity or codex_activity:
        payload["a"] = {"ts": int(scan_time)}
        if claude_activity:
            payload["a"]["cl"] = {
                "o": claude_activity["open"],
                "b": claude_activity["busy"],
                "w": claude_activity["waiting"],
            }
        if codex_activity:
            payload["a"]["cx"] = {"u": codex_activity["unread"]}
    return payload
