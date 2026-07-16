import json
from datetime import datetime, timezone
from pathlib import Path

from daemon.dashboard_collectors import (
    collect_claude_activity,
    collect_codex_activity,
    collect_codex_usage,
)


def write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value), encoding="utf-8")


def write_jsonl(path: Path, values: list[object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    text = "\n".join(json.dumps(value) for value in values) + "\n"
    path.write_text(text, encoding="utf-8")


def test_collect_claude_activity_counts_open_busy_and_waiting(tmp_path: Path) -> None:
    sessions = tmp_path / ".claude" / "sessions"
    write_json(sessions / "1.json", {"status": "busy"})
    write_json(sessions / "2.json", {"status": "idle"})
    write_json(sessions / "3.json", {"status": "shell"})
    (sessions / "broken.json").write_text("{", encoding="utf-8")

    assert collect_claude_activity(tmp_path / ".claude") == {
        "open": 3,
        "busy": 1,
        "waiting": 1,
    }


def test_collect_codex_activity_counts_unread_without_titles(tmp_path: Path) -> None:
    state = {
        "electron-persisted-atom-state": {
            "unread-thread-ids-by-host-v1": {"local": ["a", "b", "c"]}
        }
    }
    write_json(tmp_path / ".codex" / ".codex-global-state.json", state)

    assert collect_codex_activity(tmp_path / ".codex") == {"unread": 3}


def test_collect_codex_usage_reads_available_windows_and_daily_tokens(tmp_path: Path) -> None:
    now = datetime(2026, 7, 16, 18, 0, tzinfo=timezone.utc).timestamp()
    event = {
        "timestamp": "2026-07-16T17:55:00+00:00",
        "type": "event_msg",
        "payload": {
            "type": "token_count",
            "info": {"last_token_usage": {"total_tokens": 120}},
            "rate_limits": {
                "primary": {
                    "used_percent": 2,
                    "window_minutes": 10080,
                    "resets_at": int(now) + 600,
                },
                "secondary": None,
                "plan_type": "pro",
            },
        },
    }
    write_jsonl(
        tmp_path / ".codex" / "sessions" / "2026" / "07" / "16" / "rollout.jsonl",
        [event],
    )

    assert collect_codex_usage(tmp_path / ".codex", now=now) == {
        "limits": [{"percent": 2.0, "window_minutes": 10080, "reset_minutes": 10}],
        "tokens_today": 120,
        "plan": "pro",
    }


def test_missing_or_malformed_state_returns_empty_aggregates(tmp_path: Path) -> None:
    assert collect_claude_activity(tmp_path / ".claude") == {}
    assert collect_codex_activity(tmp_path / ".codex") == {}
    assert collect_codex_usage(tmp_path / ".codex", now=0) == {}
