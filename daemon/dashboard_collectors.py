from __future__ import annotations

import json
import time
from datetime import datetime
from pathlib import Path
from collections.abc import Iterator
from typing import Any


def _read_json(path: Path) -> Any | None:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError):
        return None


def collect_claude_activity(claude_home: Path) -> dict[str, int]:
    sessions_dir = claude_home / "sessions"
    if not sessions_dir.is_dir():
        return {}
    statuses: list[str] = []
    for path in sessions_dir.glob("*.json"):
        value = _read_json(path)
        if isinstance(value, dict) and isinstance(value.get("status"), str):
            statuses.append(value["status"])
    if not statuses:
        return {}
    return {
        "open": len(statuses),
        "busy": sum(status == "busy" for status in statuses),
        "waiting": sum(status == "idle" for status in statuses),
    }


def collect_codex_activity(codex_home: Path) -> dict[str, int]:
    value = _read_json(codex_home / ".codex-global-state.json")
    if not isinstance(value, dict):
        return {}
    atom = value.get("electron-persisted-atom-state")
    unread_by_host = atom.get("unread-thread-ids-by-host-v1") if isinstance(atom, dict) else None
    unread = unread_by_host.get("local") if isinstance(unread_by_host, dict) else None
    return {"unread": len(unread)} if isinstance(unread, list) else {}


def _iter_codex_events(path: Path) -> Iterator[dict[str, Any]]:
    try:
        with path.open("r", encoding="utf-8") as handle:
            for line in handle:
                try:
                    value = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if isinstance(value, dict):
                    yield value
    except (OSError, UnicodeError):
        return


def _parse_limit(value: object, now: float) -> dict[str, float | int] | None:
    if not isinstance(value, dict):
        return None
    percent = value.get("used_percent")
    window = value.get("window_minutes")
    reset = value.get("resets_at")
    if not isinstance(percent, (int, float)) or not isinstance(window, int):
        return None
    reset_minutes = -1
    if isinstance(reset, (int, float)):
        reset_minutes = max(0, int((float(reset) - now + 59) // 60))
    return {
        "percent": float(percent),
        "window_minutes": window,
        "reset_minutes": reset_minutes,
    }


def collect_codex_usage(codex_home: Path, now: float | None = None) -> dict[str, object]:
    scan_time = time.time() if now is None else now
    session_root = codex_home / "sessions"
    if not session_root.is_dir():
        return {}
    try:
        files = sorted(
            session_root.rglob("*.jsonl"),
            key=lambda path: path.stat().st_mtime,
            reverse=True,
        )
    except OSError:
        return {}
    latest_rate_limits: dict[str, Any] | None = None
    tokens_today = 0
    local_now = datetime.fromtimestamp(scan_time)
    local_day = local_now.date()
    day_start = local_now.replace(hour=0, minute=0, second=0, microsecond=0).timestamp()
    for path in files:
        try:
            modified_today = path.stat().st_mtime >= day_start
        except OSError:
            continue
        if latest_rate_limits is not None and not modified_today:
            continue
        file_rate_limits: dict[str, Any] | None = None
        for event in _iter_codex_events(path):
            payload = event.get("payload")
            if event.get("type") != "event_msg" or not isinstance(payload, dict):
                continue
            if payload.get("type") != "token_count":
                continue
            rate_limits = payload.get("rate_limits")
            if isinstance(rate_limits, dict):
                file_rate_limits = rate_limits
            if not modified_today:
                continue
            timestamp = event.get("timestamp")
            info = payload.get("info")
            last_usage = info.get("last_token_usage") if isinstance(info, dict) else None
            total = last_usage.get("total_tokens") if isinstance(last_usage, dict) else None
            try:
                event_day = datetime.fromisoformat(str(timestamp).replace("Z", "+00:00")).astimezone().date()
            except (TypeError, ValueError):
                continue
            if event_day == local_day and isinstance(total, int):
                tokens_today += max(0, total)
        if latest_rate_limits is None and file_rate_limits is not None:
            latest_rate_limits = file_rate_limits
    if latest_rate_limits is None:
        return {"tokens_today": tokens_today} if tokens_today else {}
    limits = [
        parsed
        for key in ("primary", "secondary")
        if (parsed := _parse_limit(latest_rate_limits.get(key), scan_time)) is not None
    ]
    result: dict[str, object] = {"limits": limits, "tokens_today": tokens_today}
    plan = latest_rate_limits.get("plan_type")
    if isinstance(plan, str):
        result["plan"] = plan[:11]
    return result
