#!/usr/bin/env python3
"""Unit tests for the session-awareness sidecar (daemon/clawdmeter_sessions.py).

Covers the hook state machine (including the two behaviours issue #135 calls
out as easy to get wrong), attention-first sorting, label eliding, MTU
fitting, the context-window heuristic, roster liveness, and the wire shape.

Run: python -m pytest daemon/tests/test_sessions.py -x -q
"""
import json
import os

import daemon.clawdmeter_sessions as mod
from daemon.clawdmeter_sessions import (
    STATE_STARTING, STATE_IDLE, STATE_THINKING, STATE_RESPONDING,
    STATE_RUNNING_TOOL, STATE_COMPACTING, STATE_WAITING_PERMISSION,
    STATE_WAITING_QUESTION, STATE_WAITING_INPUT, STATE_ERROR,
    SessionTable, compute_window, context_percent, elide_label,
    encode_payload, fit_payload, model_code, state_bucket, tokens_k, tool_code,
)

SID = "a3f10c2e-0000-4000-8000-000000000001"
SID2 = "7f9b0000-0000-4000-8000-000000000002"
SID3 = "c1d20000-0000-4000-8000-000000000003"


class FakeClock:
    def __init__(self, t=1000.0):
        self.t = t

    def __call__(self):
        return self.t

    def tick(self, dt=1.0):
        self.t += dt


def make_table(clock=None, **kw):
    kw.setdefault("config_dirs", [])  # no roster dirs -> liveness not enforced
    return SessionTable(now_fn=clock or FakeClock(), **kw)


def ev(name, sid=SID, **fields):
    payload = {"hook_event_name": name, "session_id": sid}
    payload.update(fields)
    return payload


def sess(table, sid=SID):
    return table.sessions[sid]


# ---------------------------------------------------------------------------
# State machine — §4.1 event table
# ---------------------------------------------------------------------------

def test_session_start_is_idle():
    t = make_table()
    assert t.handle_event(ev("SessionStart")) is True
    assert sess(t).state == STATE_IDLE


def test_user_prompt_submit_is_thinking():
    t = make_table()
    t.handle_event(ev("SessionStart"))
    t.handle_event(ev("UserPromptSubmit"))
    assert sess(t).state == STATE_THINKING


def test_pre_tool_use_is_running_tool():
    t = make_table()
    t.handle_event(ev("PreToolUse", tool_name="Bash", tool_use_id="t1"))
    s = sess(t)
    assert s.state == STATE_RUNNING_TOOL
    assert s.current_tool == "Bash"
    assert len(s.open_tools) == 1


def test_ask_user_question_is_waiting_question():
    t = make_table()
    t.handle_event(ev("PreToolUse", tool_name="AskUserQuestion", tool_use_id="q1"))
    assert sess(t).state == STATE_WAITING_QUESTION


def test_post_tool_use_returns_to_thinking_when_none_remain():
    t = make_table()
    t.handle_event(ev("PreToolUse", tool_name="Bash", tool_use_id="t1"))
    t.handle_event(ev("PostToolUse", tool_name="Bash", tool_use_id="t1"))
    s = sess(t)
    assert s.state == STATE_THINKING
    assert len(s.open_tools) == 0


def test_post_tool_use_does_not_clear_current_tool():
    """Easy-to-get-wrong #1: the tool is cleared on Stop/UserPromptSubmit,
    never on PostToolUse — otherwise the state line flickers between tools."""
    t = make_table()
    t.handle_event(ev("PreToolUse", tool_name="Bash", tool_use_id="t1"))
    t.handle_event(ev("PostToolUse", tool_name="Bash", tool_use_id="t1"))
    assert sess(t).current_tool == "Bash"
    t.handle_event(ev("Stop"))
    assert sess(t).current_tool is None
    t.handle_event(ev("PreToolUse", tool_name="Read", tool_use_id="t2"))
    t.handle_event(ev("UserPromptSubmit"))
    assert sess(t).current_tool is None
    assert len(sess(t).open_tools) == 0  # a new turn clears stale opens too


def test_ntools_counts_open_not_cumulative():
    """Easy-to-get-wrong #2: `3 tools` is the count of OPEN tool_use_ids."""
    t = make_table()
    t.handle_event(ev("PreToolUse", tool_name="Bash", tool_use_id="t1"))
    t.handle_event(ev("PreToolUse", tool_name="Read", tool_use_id="t2"))
    t.handle_event(ev("PreToolUse", tool_name="Grep", tool_use_id="t3"))
    assert len(sess(t).open_tools) == 3
    t.handle_event(ev("PostToolUse", tool_name="Read", tool_use_id="t2"))
    s = sess(t)
    assert len(s.open_tools) == 2          # not 3 (cumulative would keep counting)
    assert s.state == STATE_RUNNING_TOOL   # still running the other two


def test_post_tool_use_failure_pops_too():
    t = make_table()
    t.handle_event(ev("PreToolUse", tool_name="Bash", tool_use_id="t1"))
    t.handle_event(ev("PostToolUseFailure", tool_name="Bash", tool_use_id="t1"))
    s = sess(t)
    assert len(s.open_tools) == 0
    assert s.state == STATE_THINKING


def test_todo_write_recounts_badge():
    t = make_table()
    todos = [
        {"content": "a", "status": "completed"},
        {"content": "b", "status": "completed"},
        {"content": "c", "status": "in_progress"},
        {"content": "d", "status": "pending"},
    ]
    t.handle_event(ev("PreToolUse", tool_name="TodoWrite", tool_use_id="t1"))
    t.handle_event(ev("PostToolUse", tool_name="TodoWrite", tool_use_id="t1",
                      tool_input={"todos": todos}))
    s = sess(t)
    assert (s.tdone, s.ttotal) == (2, 4)


def test_permission_request_and_denial():
    t = make_table()
    t.handle_event(ev("PermissionRequest", tool_name="Bash", tool_use_id="t9"))
    assert sess(t).state == STATE_WAITING_PERMISSION
    t.handle_event(ev("PermissionDenied", tool_name="Bash", tool_use_id="t9"))
    assert sess(t).state == STATE_THINKING


def test_unrelated_post_tool_use_does_not_clobber_waiting_permission():
    """A chat parked on a permission prompt stays parked while a concurrent
    tool from earlier in the turn completes."""
    t = make_table()
    t.handle_event(ev("PreToolUse", tool_name="Bash", tool_use_id="t1"))
    t.handle_event(ev("PermissionRequest", tool_name="Write", tool_use_id="t2"))
    t.handle_event(ev("PostToolUse", tool_name="Bash", tool_use_id="t1"))
    assert sess(t).state == STATE_WAITING_PERMISSION


def test_notification_types():
    t = make_table()
    t.handle_event(ev("Notification", notification_type="permission_prompt"))
    assert sess(t).state == STATE_WAITING_PERMISSION
    t.handle_event(ev("Notification", notification_type="agent_needs_input"))
    assert sess(t).state == STATE_WAITING_INPUT
    t.handle_event(ev("Notification", notification_type="elicitation_dialog"))
    assert sess(t).state == STATE_WAITING_INPUT
    t.handle_event(ev("Notification", notification_type="idle_prompt"))
    assert sess(t).state == STATE_IDLE
    t.handle_event(ev("Notification", notification_type="agent_completed"))
    assert sess(t).state == STATE_IDLE
    # unmapped notification types carry no state
    t.handle_event(ev("UserPromptSubmit"))
    t.handle_event(ev("Notification", notification_type="auth_success"))
    assert sess(t).state == STATE_THINKING


def test_message_display_stop_and_stop_failure():
    t = make_table()
    t.handle_event(ev("MessageDisplay", message="hi"))
    assert sess(t).state == STATE_RESPONDING
    t.handle_event(ev("Stop"))
    assert sess(t).state == STATE_IDLE
    t.handle_event(ev("StopFailure"))
    assert sess(t).state == STATE_ERROR


def test_compaction_states():
    t = make_table()
    t.handle_event(ev("PreCompact"))
    assert sess(t).state == STATE_COMPACTING
    t.handle_event(ev("PostCompact"))
    assert sess(t).state == STATE_IDLE


def test_subagent_counts_are_live_not_cumulative():
    t = make_table()
    t.handle_event(ev("SubagentStart", agent_id="a1"))
    t.handle_event(ev("SubagentStart", agent_id="a2"))
    assert sess(t).nagents == 2
    t.handle_event(ev("SubagentStop", agent_id="a1"))
    assert sess(t).nagents == 1
    t.handle_event(ev("SubagentStop", agent_id="a2"))
    t.handle_event(ev("SubagentStop", agent_id="ghost"))  # never below zero
    assert sess(t).nagents == 0


def test_session_end_drops_immediately():
    t = make_table()
    t.handle_event(ev("SessionStart"))
    assert SID in t.sessions
    assert t.handle_event(ev("SessionEnd", reason="other")) is True
    assert SID not in t.sessions
    assert t.rows() == []  # ENDED rows never leave the host


def test_unknown_events_and_garbage_ignored():
    t = make_table()
    assert t.handle_event(ev("CwdChanged")) is False        # real but unsubscribed
    assert t.handle_event(ev("TotallyMadeUp")) is False
    assert t.handle_event({"hook_event_name": "Stop"}) is False  # no session_id
    assert t.handle_event({"session_id": SID}) is False          # no event
    assert t.handle_event("not a dict") is False
    assert t.handle_event(None) is False
    assert t.sessions == {}


def test_first_event_midstream_creates_session():
    # Listener started after the session: any known event materialises a row.
    t = make_table()
    t.handle_event(ev("PostToolUse", tool_name="Bash", tool_use_id="never-seen"))
    assert SID in t.sessions


def test_elapsed_tracks_state_changes_only():
    clock = FakeClock(1000.0)
    t = make_table(clock)
    t.handle_event(ev("UserPromptSubmit"))
    clock.tick(30)
    t.handle_event(ev("Notification", notification_type="auth_success"))  # no state change
    clock.tick(12)
    row = t.rows()[0]
    assert row[4] == 42  # THINKING since t=1000, projected at t=1042


# ---------------------------------------------------------------------------
# Sort order — §2.2: (bucket, -last_event_at)
# ---------------------------------------------------------------------------

def test_sort_waiting_working_idle():
    clock = FakeClock(0.0)
    t = make_table(clock)
    clock.t = 100; t.handle_event(ev("SessionStart", sid=SID))                # idle
    clock.t = 200; t.handle_event(ev("UserPromptSubmit", sid=SID2))           # working
    clock.t = 50;  t.handle_event(ev("PermissionRequest", sid=SID3))          # waiting
    clock.t = 300
    order = [r[0] for r in t.rows()]
    # waiting first despite being the least recent, then working, then idle
    assert order == [SID3[:2], SID2[:2], SID[:2]]


def test_sort_most_recent_first_within_bucket():
    clock = FakeClock(0.0)
    t = make_table(clock)
    clock.t = 10; t.handle_event(ev("UserPromptSubmit", sid=SID))
    clock.t = 20; t.handle_event(ev("UserPromptSubmit", sid=SID2))
    assert [r[0] for r in t.rows()] == [SID2[:2], SID[:2]]
    clock.t = 30; t.handle_event(ev("MessageDisplay", sid=SID))  # SID touched last
    assert [r[0] for r in t.rows()] == [SID[:2], SID2[:2]]


def test_bucket_mapping():
    assert state_bucket(STATE_WAITING_PERMISSION) == 0
    assert state_bucket(STATE_WAITING_QUESTION) == 0
    assert state_bucket(STATE_WAITING_INPUT) == 0
    assert state_bucket(STATE_ERROR) == 0
    assert state_bucket(STATE_THINKING) == 1
    assert state_bucket(STATE_RESPONDING) == 1
    assert state_bucket(STATE_RUNNING_TOOL) == 1
    assert state_bucket(STATE_COMPACTING) == 1
    assert state_bucket(STATE_IDLE) == 2
    assert state_bucket(STATE_STARTING) == 2


# ---------------------------------------------------------------------------
# Label eliding — §5 fitting rule 1
# ---------------------------------------------------------------------------

def test_elide_noop_when_short():
    assert elide_label("weblab", 8) == "weblab"
    assert elide_label("12345678", 8) == "12345678"


def test_elide_middle_preserves_trailing_discriminator():
    a = elide_label("clawdmeter-36", 8)
    b = elide_label("clawdmeter-2c", 8)
    assert len(a) == 8 and len(b) == 8
    assert a != b                       # must not both become "clawdmete…"
    assert a.endswith("-36") and b.endswith("-2c")
    assert a.startswith("claw") and mod.ELLIPSIS in a


def test_elide_floor_is_eight_chars():
    label = "a-very-long-session-name-42"
    assert len(elide_label(label, 8)) == 8
    for cap in (10, 14, 20):
        out = elide_label(label, cap)
        assert len(out) == cap
        assert out.endswith(label[-((cap - 1) // 2):])


# ---------------------------------------------------------------------------
# MTU fitting — §5: shrink labels first, then drop rows from the tail
# ---------------------------------------------------------------------------

def _row(sid, label, state=STATE_THINKING):
    return [sid, label, state, 50, 10, 2, 0, 0, 0, 0, 0, 100]


FIT_ROWS = [
    _row("a3", "clawdmeter-36", STATE_WAITING_PERMISSION),
    _row("7f", "ordora-platform"),
    _row("c1", "weblab-site"),
    _row("2e", "notes-vault", STATE_IDLE),
]


def test_fit_generous_budget_keeps_everything():
    payload = fit_payload(FIT_ROWS, 4096)
    rows = json.loads(payload)["ss"]
    assert [r[1] for r in rows] == [r[1] for r in FIT_ROWS]  # labels untouched


def test_fit_shrinks_labels_before_dropping_rows():
    full = len(fit_payload(FIT_ROWS, 4096).encode("utf-8"))
    payload = fit_payload(FIT_ROWS, full - 4)  # just too small for full labels
    rows = json.loads(payload)["ss"]
    assert len(rows) == len(FIT_ROWS)          # no row dropped...
    assert any(mod.ELLIPSIS in r[1] for r in rows)  # ...a label shrank instead


def test_fit_drops_rows_from_tail_only():
    payload = fit_payload(FIT_ROWS, 120)
    rows = json.loads(payload)["ss"]
    assert 0 < len(rows) < len(FIT_ROWS)
    # surviving sids are a strict prefix of the sorted input: the waiting chat
    # in slot 0 is never the one dropped
    assert [r[0] for r in rows] == [r[0] for r in FIT_ROWS][: len(rows)]
    assert rows[0][0] == "a3"


def test_fit_payload_is_compact_json():
    payload = fit_payload(FIT_ROWS, 4096)
    assert ": " not in payload and ", " not in payload
    assert payload.startswith('{"ss":[[')


def test_fit_impossible_budget_yields_empty_list():
    assert fit_payload(FIT_ROWS, 10) == '{"ss":[]}'
    assert encode_payload([]) == '{"ss":[]}'


def test_fit_respects_utf8_byte_budget():
    rows = [_row("a3", "sessión-namé-with-utf8-36")]
    for budget in (60, 70, 80):
        assert len(fit_payload(rows, budget).encode("utf-8")) <= budget


# ---------------------------------------------------------------------------
# Context window heuristic — §4.3
# ---------------------------------------------------------------------------

def test_window_default_200k():
    assert compute_window(150_000, "claude-sonnet-4-5") == 200_000


def test_window_1m_marker():
    assert compute_window(150_000, "claude-sonnet-4-5[1m]") == 1_000_000


def test_window_snaps_up_when_observed_exceeds_assumption():
    assert compute_window(250_000, "claude-sonnet-4-5") == 1_000_000
    assert compute_window(1_200_000, "claude-sonnet-4-5") == 2_000_000
    assert compute_window(1_200_000, "claude-opus-4[1m]") == 2_000_000


def test_pinned_window_disables_snap_up():
    assert compute_window(250_000, "claude-sonnet-4-5", pinned_k=200) == 200_000
    assert compute_window(50_000, "claude-sonnet-4-5[1m]", pinned_k=500) == 500_000


def test_context_percent():
    assert context_percent(None, "claude-opus-4") == -1
    assert context_percent(100_000, "claude-sonnet-4-5") == 50
    assert context_percent(190_000, "claude-sonnet-4-5") == 95
    assert context_percent(250_000, "claude-sonnet-4-5") == 25   # snapped to 1M
    assert context_percent(250_000, "claude-sonnet-4-5", pinned_k=200) == 100  # clamped


def test_context_read_from_transcript(tmp_path):
    transcript = tmp_path / "t.jsonl"
    lines = [
        {"type": "user", "message": {"content": "hi"}},
        {"type": "assistant", "isSidechain": False,
         "message": {"model": "claude-fable-5",
                     "usage": {"input_tokens": 100_000,
                               "cache_read_input_tokens": 50_000,
                               "cache_creation_input_tokens": 10_000,
                               "output_tokens": 42}}},
        # newer, but a subagent turn — must be skipped
        {"type": "assistant", "isSidechain": True,
         "message": {"model": "claude-haiku-4-5",
                     "usage": {"input_tokens": 999_999}}},
    ]
    transcript.write_text("\n".join(json.dumps(l) for l in lines) + "\n")
    t = make_table()
    t.handle_event(ev("SessionStart", transcript_path=str(transcript)))
    s = sess(t)
    assert s.ctx == 80          # 160k of 200k
    assert s.tok == 160         # same read, absolute: 160,000 tokens -> 160
    assert s.model == 4         # fable


def test_context_unknown_when_transcript_missing():
    t = make_table()
    t.handle_event(ev("SessionStart", transcript_path="/nonexistent/nope.jsonl"))
    assert sess(t).ctx == -1
    assert sess(t).tok == -1    # tok is -1 exactly when ctx is -1


# ---------------------------------------------------------------------------
# tok — absolute context tokens in 1k units, consistent with ctx
# ---------------------------------------------------------------------------

def test_tokens_k_units_and_rounding():
    assert tokens_k(None) == -1
    assert tokens_k(0) == 0
    assert tokens_k(160_000) == 160
    assert tokens_k(160_499) == 160     # round to nearest...
    assert tokens_k(160_500) == 161     # ...half rounds up, deterministically
    assert tokens_k(1_234_567) == 1235


def _table_with_transcript(tmp_path, tokens):
    transcript = tmp_path / "t.jsonl"
    rec = {"type": "assistant", "isSidechain": False,
           "message": {"model": "claude-sonnet-4-5",
                       "usage": {"input_tokens": tokens}}}
    transcript.write_text(json.dumps(rec) + "\n")
    t = make_table()
    t.handle_event(ev("SessionStart", transcript_path=str(transcript)))
    return t


def test_tok_known_ctx_correct_k_value(tmp_path):
    t = _table_with_transcript(tmp_path, 412_345)
    s = sess(t)
    assert s.tok == 412                 # 412k, NOT divided by the window
    assert s.ctx == 41                  # 412,345 of a snapped-up 1M window


def test_tok_and_ctx_come_from_the_same_read(tmp_path):
    t = _table_with_transcript(tmp_path, 100_000)
    assert (sess(t).ctx, sess(t).tok) == (50, 100)
    # transcript disappears; Stop triggers a re-read -> both go unknown together
    os.remove(sess(t).transcript_path)
    t.handle_event(ev("Stop"))
    assert (sess(t).ctx, sess(t).tok) == (-1, -1)


def test_tok_on_the_wire_at_index_11(tmp_path):
    t = _table_with_transcript(tmp_path, 190_000)
    row = json.loads(t.project(4096))["ss"][0]
    assert row[3] == 95                 # ctx
    assert row[11] == 190               # tok appended at the end


# ---------------------------------------------------------------------------
# Wire format shape — §5
# ---------------------------------------------------------------------------

def test_wire_row_shape_and_codes():
    clock = FakeClock(1000.0)
    t = make_table(clock)
    t.handle_event(ev("SessionStart", cwd="/home/x/clawdmeter"))
    t.handle_event(ev("UserPromptSubmit"))
    t.handle_event(ev("PreToolUse", tool_name="Bash", tool_use_id="t1"))
    t.handle_event(ev("SubagentStart", agent_id="a1"))
    clock.tick(62)
    rows = json.loads(t.project(4096))["ss"]
    assert len(rows) == 1
    row = rows[0]
    assert len(row) == 12
    (sid, label, state, ctx, elapsed, model, tool,
     ntools, nagents, tdone, ttotal, tok) = row
    assert sid == SID[:2] and len(sid) == 2
    assert label == "clawdmeter"          # basename(cwd) fallback
    assert state == STATE_RUNNING_TOOL
    assert ctx == -1                       # no transcript -> bar hidden, not empty
    assert elapsed == 62
    assert model == 0
    assert tool == 1                       # Bash
    assert (ntools, nagents, tdone, ttotal) == (1, 1, 0, 0)
    assert tok == -1                       # unknown together with ctx


def test_model_and_tool_code_tables():
    assert model_code("claude-opus-4-1") == 1
    assert model_code("claude-sonnet-4-5[1m]") == 2
    assert model_code("claude-haiku-4-5") == 3
    assert model_code("claude-fable-5") == 4
    assert model_code("gpt-oss") == 0
    assert model_code(None) == 0
    for name, code in (("Bash", 1), ("Read", 2), ("Edit", 3), ("Write", 4),
                       ("Grep", 5), ("Glob", 6), ("Task", 7),
                       ("WebFetch", 8), ("WebSearch", 9)):
        assert tool_code(name) == code
    assert tool_code("SomeMcpTool") == 0
    assert tool_code(None) == 0


# ---------------------------------------------------------------------------
# Roster liveness — §4.2: grace, unreadable roster, name pickup
# ---------------------------------------------------------------------------

def _write_roster(cdir, session_id, pid, name=None):
    sdir = cdir / "sessions"
    sdir.mkdir(parents=True, exist_ok=True)
    rec = {"pid": pid, "sessionId": session_id}
    if name:
        rec["name"] = name
    (sdir / f"{pid}.json").write_text(json.dumps(rec))


def test_roster_absence_dropped_only_after_grace(tmp_path):
    clock = FakeClock(1000.0)
    t = SessionTable(config_dirs=[str(tmp_path)], now_fn=clock)
    (tmp_path / "sessions").mkdir()   # roster readable, but no entry for us
    t.handle_event(ev("PermissionRequest"))
    assert t.sweep() is False          # grace started, nothing wire-visible
    assert SID in t.sessions           # 30s grace: first hook may beat the file
    clock.tick(mod.ROSTER_GRACE_S + 1)
    assert t.sweep() is True
    assert SID not in t.sessions


def test_hook_resets_roster_grace(tmp_path):
    clock = FakeClock(1000.0)
    t = SessionTable(config_dirs=[str(tmp_path)], now_fn=clock)
    (tmp_path / "sessions").mkdir()
    t.handle_event(ev("UserPromptSubmit"))
    t.sweep()                          # grace starts
    clock.tick(mod.ROSTER_GRACE_S - 5)
    t.handle_event(ev("MessageDisplay"))   # proof of life
    clock.tick(10)
    t.sweep()
    assert SID in t.sessions           # grace restarted by the hook


def test_unreadable_roster_never_drops_a_session(tmp_path):
    """§4.2/§9: a chat blocked on a permission prompt is silent — no roster
    means sessions linger (6h backstop), they never disappear wrongly."""
    clock = FakeClock(1000.0)
    t = SessionTable(config_dirs=[str(tmp_path / "missing")], now_fn=clock)
    t.handle_event(ev("PermissionRequest"))
    clock.tick(3600.0)                 # an hour of silence on the prompt
    assert t.sweep() is False
    assert t.sessions[SID].state == STATE_WAITING_PERMISSION


def test_stale_sweep_backstop(tmp_path):
    clock = FakeClock(1000.0)
    t = SessionTable(config_dirs=[str(tmp_path / "missing")], now_fn=clock)
    t.handle_event(ev("SessionStart"))
    clock.tick(mod.STALE_SWEEP_S + 1)
    assert t.sweep() is True
    assert t.sessions == {}


def test_roster_supplies_label_and_liveness(tmp_path):
    clock = FakeClock(1000.0)
    t = SessionTable(config_dirs=[str(tmp_path)], now_fn=clock)
    _write_roster(tmp_path, SID, os.getpid(), name="clawdmeter-c5")
    t.handle_event(ev("UserPromptSubmit", cwd="/home/x/clawdmeter"))
    assert t.sweep() is True           # roster name arrived: wire-visible change
    assert t.sessions[SID].label() == "clawdmeter-c5"
    clock.tick(3600.0)
    assert t.sweep() is False          # alive pid: silence never retires it
    assert SID in t.sessions


def test_roster_entry_with_dead_pid_is_not_liveness(tmp_path):
    # Roster files survive crashes; a record pointing at a dead pid must not
    # vouch for the session.
    clock = FakeClock(1000.0)
    t = SessionTable(config_dirs=[str(tmp_path)], now_fn=clock)
    _write_roster(tmp_path, SID, None)     # unparseable pid == dead
    t.handle_event(ev("SessionStart"))
    t.sweep()
    clock.tick(mod.ROSTER_GRACE_S + 1)
    assert t.sweep() is True
    assert SID not in t.sessions
