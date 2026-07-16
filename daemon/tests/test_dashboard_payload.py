import json
from pathlib import Path

from daemon.dashboard_payload import build_dashboard_payload


def test_build_dashboard_payload_preserves_claude_and_compacts_local_data(
    monkeypatch,
    tmp_path: Path,
) -> None:
    monkeypatch.setattr(
        "daemon.dashboard_payload.collect_codex_usage",
        lambda _home, now=None: {
            "limits": [{"percent": 2.0, "window_minutes": 10080, "reset_minutes": 10}],
            "tokens_today": 120,
            "plan": "pro",
        },
    )
    monkeypatch.setattr(
        "daemon.dashboard_payload.collect_claude_activity",
        lambda _home: {
            "open": 3,
            "busy": 1,
            "waiting": 1,
            "session_name": "SECRET_SESSION",
        },
    )
    monkeypatch.setattr(
        "daemon.dashboard_payload.collect_codex_activity",
        lambda _home: {"unread": 5, "title": "SECRET_TITLE"},
    )

    payload = build_dashboard_payload({"s": 41, "w": 12, "ok": True}, tmp_path, now=1000)

    assert payload == {
        "s": 41,
        "w": 12,
        "ok": True,
        "v": 2,
        "ts": 1000,
        "x": {"l": [{"p": 2.0, "wm": 10080, "rm": 10}], "td": 120, "pl": "pro"},
        "a": {"cl": {"o": 3, "b": 1, "w": 1}, "cx": {"u": 5}, "ts": 1000},
    }
    wire = json.dumps(payload, separators=(",", ":"))
    assert len(wire.encode("utf-8")) < 768
    for forbidden in (
        "SECRET_SESSION",
        "SECRET_TITLE",
        "prompt",
        "response",
        "project",
        str(tmp_path),
    ):
        assert forbidden not in wire


def test_local_data_is_still_sent_when_claude_poll_is_unavailable(monkeypatch, tmp_path: Path) -> None:
    monkeypatch.setattr("daemon.dashboard_payload.collect_codex_usage", lambda _home, now=None: {})
    monkeypatch.setattr(
        "daemon.dashboard_payload.collect_claude_activity",
        lambda _home: {"open": 1, "busy": 0, "waiting": 1},
    )
    monkeypatch.setattr("daemon.dashboard_payload.collect_codex_activity", lambda _home: {"unread": 0})

    payload = build_dashboard_payload(None, tmp_path, now=2000)

    assert "s" not in payload
    assert payload["x"] is None
    assert payload["a"] == {
        "ts": 2000,
        "cl": {"o": 1, "b": 0, "w": 1},
        "cx": {"u": 0},
    }


def test_unavailable_local_collectors_emit_explicit_tombstones(monkeypatch, tmp_path: Path) -> None:
    monkeypatch.setattr("daemon.dashboard_payload.collect_codex_usage", lambda _home, now=None: {})
    monkeypatch.setattr("daemon.dashboard_payload.collect_claude_activity", lambda _home: {})
    monkeypatch.setattr("daemon.dashboard_payload.collect_codex_activity", lambda _home: {})

    payload = build_dashboard_payload(None, tmp_path, now=3000)

    assert payload["x"] is None
    assert payload["a"] == {"ts": 3000, "cl": None, "cx": None}
