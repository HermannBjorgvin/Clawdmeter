#!/usr/bin/env python3
"""Unit tests for the running-agents scan (collect_agents / agents_payload).

Run: python -m pytest daemon/tests/test_agents.py -x -q
"""
import json
import os

import daemon.claude_usage_daemon as mod
from daemon.claude_usage_daemon import (DoneTracker, agents_payload, collect_agents, describe_tool,
                                         last_activity, sessions_signature)

NOW_MS = 1_790_000_000_000
DEAD_PID = 999_999_999   # far above any real PID


def _session(d, pid, **kw):
    (d / "sessions").mkdir(parents=True, exist_ok=True)
    info = {"pid": pid, "cwd": "/Users/x/proj", "status": "idle",
            "statusUpdatedAt": NOW_MS, **kw}
    (d / "sessions" / f"{pid}.json").write_text(json.dumps(info))


def test_lists_live_sessions_with_state_and_minutes(tmp_path):
    _session(tmp_path, os.getpid(), name="clawdmeter-c5", status="busy",
             statusUpdatedAt=NOW_MS - 12 * 60_000)
    assert collect_agents([tmp_path], NOW_MS) == [("clawdmeter-c5", "b", 12, "")]


def test_skips_sessions_whose_process_is_gone(tmp_path):
    _session(tmp_path, DEAD_PID, name="stale")
    assert collect_agents([tmp_path], NOW_MS) == []


def test_falls_back_to_cwd_basename_and_sanitizes_to_ascii(tmp_path):
    _session(tmp_path, os.getpid(), cwd="/Users/x/café")
    assert collect_agents([tmp_path], NOW_MS) == [("caf?", "i", 0, "")]


def test_orders_waiting_then_working_then_idle(tmp_path, monkeypatch):
    monkeypatch.setattr(mod, "_pid_alive", lambda pid: True)
    _session(tmp_path, 101, name="idle", status="idle")
    _session(tmp_path, 102, name="wait", status="waiting_for_input")
    _session(tmp_path, 103, name="busy", status="busy")
    assert [a[1] for a in collect_agents([tmp_path], NOW_MS)] == ["w", "b", "i"]


def test_missing_sessions_dir_is_empty(tmp_path):
    assert collect_agents([tmp_path], NOW_MS) == []


def test_payload_caps_rows_but_reports_total():
    agents = [(f"agent-{i}", "b", i, "") for i in range(7)]
    p = agents_payload(agents)
    assert len(p["ag"]) == mod.AGENTS_MAX and p["n"] == 7


def test_payload_fits_one_ble_write():
    agents = [("x" * 40, "b", 99999, "Edit a_really_long_file_name.cpp")] * 4
    p = agents_payload(agents)
    assert len(json.dumps(p, separators=(",", ":"))) <= mod.AGENTS_PAYLOAD_MAX
    assert all(len(r[0]) <= mod.AGENT_NAME_MAX for r in p["ag"])


# ---------------------------------------------------------------------------
# activity hints
# ---------------------------------------------------------------------------

def _transcript(path, *entries):
    lines = ['{"cut mid-line', *(json.dumps(e) for e in entries)]
    path.write_text("\n".join(lines) + "\n")
    return path


def _assistant(*blocks):
    return {"type": "assistant", "message": {"role": "assistant", "content": list(blocks)}}


def test_describe_tool_names_the_file_or_command():
    assert describe_tool("Edit", {"file_path": "/a/b/ui.cpp"}) == "Edit ui.cpp"
    assert describe_tool("Bash", {"command": "FOO=1 /opt/homebrew/bin/pio run -e x"}) == "Run pio"
    assert describe_tool("Grep", {"pattern": "TODO"}) == "Search TODO"
    assert describe_tool("Bash", {"command": "cd /x/y && P=$(ls /dev/cu.*) ; pio run"}) == "Run ls"
    assert describe_tool("Bash", {"command": "cd /x && ./screenshot.sh out.png"}) == "Run screenshot.sh"
    assert describe_tool("Bash", {"command": "for i in 1 2; do ./screenshot.sh a$i.png; sleep 2; done"}) == "Run screenshot.sh"
    assert describe_tool("Bash", {"command": "while true; do pio run; done"}) == "Run pio"
    assert describe_tool("Bash", {"command": "if [ -f x ]; then make; fi"}) == "Run make"
    assert describe_tool("mcp__github__create_pr", {}) == "create pr"


def test_last_activity_is_newest_tool_call(tmp_path):
    t = _transcript(tmp_path / "s.jsonl",
                    _assistant({"type": "text", "text": "hi"}),
                    _assistant({"type": "tool_use", "name": "Read", "input": {"file_path": "/x/main.cpp"}}),
                    {"type": "attachment"})
    assert last_activity(t) == "Read main.cpp"


def test_last_activity_after_tool_result_is_thinking(tmp_path):
    t = _transcript(tmp_path / "s.jsonl",
                    _assistant({"type": "tool_use", "name": "Bash", "input": {"command": "ls"}}),
                    {"type": "user", "message": {"role": "user", "content": [{"type": "tool_result"}]}})
    assert last_activity(t) == "Thinking"


def test_busy_session_carries_activity_idle_does_not(tmp_path, monkeypatch):
    monkeypatch.setattr(mod, "_TRANSCRIPTS", {})
    monkeypatch.setattr(mod, "_pid_alive", lambda pid: True)
    proj = tmp_path / "projects" / "-x-proj"
    proj.mkdir(parents=True)
    tool = _assistant({"type": "tool_use", "name": "Edit", "input": {"file_path": "/x/ui.cpp"}})
    _transcript(proj / "S1.jsonl", tool)
    _transcript(proj / "S2.jsonl", tool)
    _session(tmp_path, 201, name="busy", status="busy", sessionId="S1")
    _session(tmp_path, 202, name="idle", status="idle", sessionId="S2")
    assert collect_agents([tmp_path], NOW_MS) == [("busy", "b", 0, "Edit ui.cpp"), ("idle", "i", 0, "")]


def test_payload_shrinks_activity_before_dropping_rows():
    agents = [(f"agent-{i}", "b", 5, "Edit some_long_name.cpp") for i in range(4)]
    p = agents_payload(agents, limit=160)
    assert len(p["ag"]) == 4
    assert len(json.dumps(p, separators=(",", ":"))) <= 160


def test_waiting_session_says_what_it_needs(tmp_path, monkeypatch):
    monkeypatch.setattr(mod, "_TRANSCRIPTS", {})
    monkeypatch.setattr(mod, "_pid_alive", lambda pid: True)
    proj = tmp_path / "projects" / "-x-proj"
    proj.mkdir(parents=True)
    _transcript(proj / "S1.jsonl", _assistant({"type": "tool_use", "name": "Bash", "input": {"command": "rm -rf build"}}))
    _session(tmp_path, 301, name="perm", status="waiting", waitingFor="permission prompt", sessionId="S1")
    _session(tmp_path, 302, name="ask", status="waiting", waitingFor="input needed")
    acts = {a[0]: a[3] for a in collect_agents([tmp_path], NOW_MS)}
    assert acts == {"perm": "Allow Run rm", "ask": "Needs your input"}


# ---------------------------------------------------------------------------
# done tracking + registry watch
# ---------------------------------------------------------------------------

def test_busy_to_idle_shows_done_then_expires():
    t = DoneTracker()
    t.apply([("a", "b", 3, "Edit x.c")], now=0)
    assert t.apply([("a", "i", 0, "")], now=1) == [("a", "d", 0, "Finished")]
    assert t.apply([("a", "i", 1, "")], now=1 + mod.DONE_HOLD_S) == [("a", "i", 1, "")]


def test_idle_from_the_start_is_not_done():
    t = DoneTracker()
    assert t.apply([("a", "i", 5, "")], now=0) == [("a", "i", 5, "")]


def test_done_clears_when_busy_again_and_sorts_before_working():
    t = DoneTracker()
    t.apply([("a", "b", 0, ""), ("b", "b", 0, "")], now=0)
    rows = t.apply([("b", "b", 0, "Run pio"), ("a", "i", 0, "")], now=1)
    assert [(r[0], r[1]) for r in rows] == [("a", "d"), ("b", "b")]
    assert t.apply([("a", "b", 0, "")], now=2)[0][1] == "b"


def test_signature_changes_when_a_session_file_is_rewritten(tmp_path):
    _session(tmp_path, 401, status="busy")
    before = sessions_signature([tmp_path])
    f = tmp_path / "sessions" / "401.json"
    os.utime(f, ns=(0, f.stat().st_mtime_ns + 1_000_000))
    assert sessions_signature([tmp_path]) != before


# ---------------------------------------------------------------------------
# heartbeat ageing
# ---------------------------------------------------------------------------

def test_age_payload_shrinks_countdowns_and_advances_clock():
    p = {"s": 10, "sr": 100, "w": 5, "wr": 2, "t": 1000, "ok": True}
    aged = mod.age_payload(p, 185)
    assert aged == {"s": 10, "sr": 97, "w": 5, "wr": 0, "t": 1185, "ok": True}
    assert p["sr"] == 100   # original untouched
