#!/usr/bin/env python3
"""Unit tests for the Claude Code hook listener -- HOOK-01.

Covers config parsing, the event state machine, transcript context reading, the
BLE fragment byte budget, and the HTTP endpoint end to end over a real loopback
socket. No network beyond loopback, no Claude Code process needed.

Run: python -m pytest daemon/tests/test_hook_listener.py -x -q
"""
import asyncio
import json
import os

import pytest

from daemon.hook_listener import (
    _pid_alive,
    scan_live_sessions,
    BLE_LABEL_CHARS,
    BLE_LABEL_CHARS_MIN,
    BLE_MAX_ROWS,
    BLE_ROWS_BUDGET_BYTES,
    DEFAULT_PORT,
    HookListener,
    SessionState,
    SessionTable,
    ble_fragment_size,
    ble_label,
    budget_from_mtu,
    infer_context_limit,
    model_family,
    read_context_usage,
    read_hook_port,
)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _run(coro):
    """Run a coroutine synchronously.

    Fresh loop per call for the same reason as the poll tests: another test file
    calling asyncio.run() closes the process-default loop, which would make
    these order-dependent.
    """
    loop = asyncio.new_event_loop()
    try:
        return loop.run_until_complete(coro)
    finally:
        loop.close()


def _event(name, session_id="s1", **extra):
    """Build a hook payload with the fields every event carries."""
    payload = {
        "session_id": session_id,
        "hook_event_name": name,
        "cwd": "/home/user/netmap",
        "permission_mode": "default",
        "effort": {"level": "high"},
    }
    payload.update(extra)
    return payload


def _write_transcript(tmp_path, records):
    path = tmp_path / "transcript.jsonl"
    path.write_text(
        "\n".join(json.dumps(r) for r in records) + "\n", encoding="utf-8"
    )
    return path


def _assistant_rec(tokens, model="claude-opus-5", sidechain=False):
    return {
        "type": "assistant",
        "isSidechain": sidechain,
        "message": {
            "role": "assistant",
            "model": model,
            "usage": {
                "input_tokens": 1,
                "cache_read_input_tokens": tokens - 1,
                "cache_creation_input_tokens": 0,
            },
        },
    }


# ---------------------------------------------------------------------------
# Config
# ---------------------------------------------------------------------------

def test_hook_port_absent_file_disabled(tmp_path):
    assert read_hook_port(tmp_path / "nope") is None


def test_hook_port_default_is_disabled(tmp_path):
    """An upgrade must never silently open a listening socket -- matches the
    `chime = off` / `clock = off` convention."""
    cfg = tmp_path / "config"
    cfg.write_text("chime = off\nclock = 24\n")
    assert read_hook_port(cfg) is None


@pytest.mark.parametrize("raw,expected", [
    ("hook_port = 25293", 25293),
    ("hook_port = 8080", 8080),
    ("hook_port=on", DEFAULT_PORT),
    ("hook_port = true", DEFAULT_PORT),
    ("HOOK_PORT = 25293", 25293),
    ("hook_port = off", None),
    ("hook_port = 0", None),
    ("hook_port = false", None),
    ("hook_port = nonsense", None),
    ("hook_port = 70000", None),
    ("hook_port = -1", None),
    ("hook_port = 25293  # clawd on a keypad", 25293),
    ("# hook_port = 25293", None),
])
def test_hook_port_parsing(tmp_path, raw, expected):
    cfg = tmp_path / "config"
    cfg.write_text(raw + "\n")
    assert read_hook_port(cfg) == expected


# ---------------------------------------------------------------------------
# Ingest / state machine
# ---------------------------------------------------------------------------

def test_ingest_creates_session_and_label():
    t = SessionTable()
    s = t.ingest(_event("SessionStart", source="startup"))
    assert s is not None
    assert s.session_id == "s1"
    assert s.project == "netmap"
    assert s.label == "netmap"
    assert s.state is SessionState.IDLE
    assert s.effort == "high"
    assert len(t) == 1


def test_label_handles_windows_cwd():
    """cwd arrives with backslashes on Windows; PurePath would pick the wrong
    flavour when the daemon parses it on POSIX."""
    t = SessionTable()
    s = t.ingest(_event("SessionStart", cwd=r"c:\Users\fred\src\Clawdmeter"))
    assert s.project == "Clawdmeter"


@pytest.mark.parametrize("event,extra,expected", [
    ("UserPromptSubmit", {}, SessionState.THINKING),
    ("PermissionRequest", {"tool_name": "Bash"}, SessionState.WAITING_PERMISSION),
    ("MessageDisplay", {}, SessionState.RESPONDING),
    ("Stop", {}, SessionState.IDLE),
    ("StopFailure", {"error_type": "rate_limit"}, SessionState.ERROR),
    ("PreCompact", {}, SessionState.COMPACTING),
    ("SessionEnd", {"end_reason": "clear"}, SessionState.ENDED),
    ("Elicitation", {}, SessionState.WAITING_INPUT),
    ("PreToolUse", {"tool_name": "Read"}, SessionState.RUNNING_TOOL),
])
def test_state_transitions(event, extra, expected):
    t = SessionTable()
    s = t.ingest(_event(event, **extra))
    assert s.state is expected


def test_askuserquestion_is_a_wait_not_work():
    """The transcript cannot distinguish this from a running tool; the hook can."""
    t = SessionTable()
    s = t.ingest(_event("PreToolUse", tool_name="AskUserQuestion"))
    assert s.state is SessionState.WAITING_QUESTION
    assert s.needs_attention


@pytest.mark.parametrize("ntype,expected", [
    ("permission_prompt", SessionState.WAITING_PERMISSION),
    ("idle_prompt", SessionState.IDLE),
    ("agent_needs_input", SessionState.WAITING_INPUT),
    ("agent_completed", SessionState.IDLE),
    ("elicitation_dialog", SessionState.WAITING_INPUT),
])
def test_notification_mapping(ntype, expected):
    t = SessionTable()
    s = t.ingest(_event("Notification", notification_type=ntype,
                        message="needs you"))
    assert s.state is expected
    assert s.last_notification_type == ntype
    assert s.last_notification_message == "needs you"


def test_permission_gap_the_transcript_cannot_see():
    """PermissionRequest -> PostToolUse is the state that has no on-disk
    representation in the transcript at all."""
    t = SessionTable()
    t.ingest(_event("PreToolUse", tool_name="Bash"))
    s = t.ingest(_event("PermissionRequest", tool_name="Bash"))
    assert s.state is SessionState.WAITING_PERMISSION
    s = t.ingest(_event("PostToolUse", tool_name="Bash"))
    assert s.state is SessionState.THINKING
    assert s.pending_tool_name == ""


def test_stop_records_message_and_counts_turns():
    t = SessionTable()
    t.ingest(_event("Stop", last_assistant_message="done"))
    s = t.ingest(_event("Stop", last_assistant_message="done again"))
    assert s.turns == 2
    assert s.last_assistant_message == "done again"


def test_stopfailure_captures_rate_limit():
    t = SessionTable()
    s = t.ingest(_event("StopFailure", error_type="rate_limit",
                        error_message="Rate limit exceeded"))
    assert s.error_type == "rate_limit"
    assert s.errors == 1
    assert s.needs_attention


def test_subagent_and_task_counters():
    t = SessionTable()
    t.ingest(_event("SubagentStart"))
    t.ingest(_event("SubagentStart"))
    s = t.ingest(_event("SubagentStop"))
    assert s.active_subagents == 1
    assert s.subagents_started == 2
    t.ingest(_event("TaskCreated"))
    s = t.ingest(_event("TaskCompleted"))
    assert s.tasks_created == 1 and s.tasks_completed == 1


def test_unknown_event_is_ignored_not_fatal():
    """A future Claude Code release adding events must not break the listener."""
    t = SessionTable()
    t.ingest(_event("Stop"))
    s = t.ingest(_event("SomeFutureEventName"))
    assert s.state is SessionState.IDLE          # unchanged
    assert ("SomeFutureEventName" in [h[1] for h in s.history])


@pytest.mark.parametrize("payload", [
    None, [], "nope", 42,
    {},                                          # no session_id
    {"session_id": "s1"},                         # no event name
    {"hook_event_name": "Stop"},                  # no session_id
    {"session_id": "", "hook_event_name": "Stop"},
    {"session_id": 5, "hook_event_name": "Stop"},
])
def test_malformed_payloads_rejected_without_raising(payload):
    t = SessionTable()
    assert t.ingest(payload) is None
    assert len(t) == 0


def test_state_since_stamps_only_on_transition():
    """'waiting 4m' must measure the wait, not the time since the last repeat."""
    t = SessionTable()
    s = t.ingest(_event("Notification", notification_type="permission_prompt"))
    first = s.state_since
    t.ingest(_event("Notification", notification_type="permission_prompt"))
    assert s.state_since == first


def test_ended_sessions_leave_active_list():
    t = SessionTable()
    t.ingest(_event("SessionStart", session_id="a"))
    t.ingest(_event("SessionStart", session_id="b"))
    assert len(t.active()) == 2
    t.ingest(_event("SessionEnd", session_id="b", end_reason="clear"))
    labels = [s.session_id for s in t.active()]
    assert labels == ["a"]


def test_attention_sessions_sort_first():
    """Ordering decides who survives BLE truncation, so a session that wants you
    must never be the one dropped."""
    t = SessionTable()
    for i in range(6):
        t.ingest(_event("Stop", session_id=f"s{i}", cwd=f"/p/proj{i}"))
    t.ingest(_event("PermissionRequest", session_id="s0", cwd="/p/proj0",
                    tool_name="Bash"))
    ordered = t.active()
    assert ordered[0].session_id == "s0"
    assert ordered[0].needs_attention


def test_max_sessions_cap_evicts_least_recent():
    t = SessionTable(max_sessions=3)
    for i in range(10):
        t.ingest(_event("Stop", session_id=f"s{i}", cwd=f"/p/proj{i}"))
    assert len(t) <= 3


# ---------------------------------------------------------------------------
# Transcript / context
# ---------------------------------------------------------------------------

def test_read_context_usage_sums_the_three_fields(tmp_path):
    path = _write_transcript(tmp_path, [_assistant_rec(50_000)])
    tokens, limit, model = read_context_usage(path)
    assert tokens == 50_000
    assert limit == 200_000
    assert model == "claude-opus-5"


def test_read_context_usage_takes_newest(tmp_path):
    path = _write_transcript(tmp_path, [
        _assistant_rec(10_000), _assistant_rec(90_000),
    ])
    tokens, _, _ = read_context_usage(path)
    assert tokens == 90_000


def test_read_context_usage_skips_sidechain(tmp_path):
    """Subagent turns share the file and would inflate the figure."""
    path = _write_transcript(tmp_path, [
        _assistant_rec(70_000),
        _assistant_rec(500_000, sidechain=True),
    ])
    tokens, _, _ = read_context_usage(path)
    assert tokens == 70_000


def test_read_context_usage_infers_1m_limit(tmp_path):
    path = _write_transcript(tmp_path, [
        _assistant_rec(300_000, model="claude-opus-5[1m]"),
    ])
    tokens, limit, _ = read_context_usage(path)
    assert (tokens, limit) == (300_000, 1_000_000)


def test_long_1m_session_is_not_reported_as_full(tmp_path):
    """Observed on hardware: a 1M session showed 100%.

    Transcripts record `message.model` with the "[1m]" suffix stripped, so the
    model name alone cannot distinguish a 1M window from a 200k one. 440k tokens
    against a 200k limit clamps to 100%; against the inferred 1M it reads 44%,
    which is what Claude Code's own status line showed at the time.
    """
    path = _write_transcript(tmp_path, [
        _assistant_rec(440_000, model="claude-opus-5"),   # note: no "[1m]"
    ])
    tokens, limit, _ = read_context_usage(path)
    assert tokens == 440_000
    assert limit == 1_000_000

    t = SessionTable()
    s = t.ingest(_event("Stop", transcript_path=str(path)))
    assert s.context_pct == 44


def test_transcript_supplies_model_when_hooks_did_not(tmp_path):
    """A session already running when the hooks were installed never sends a
    SessionStart, so its rows went out with model code 0 (unknown)."""
    path = _write_transcript(tmp_path, [_assistant_rec(10_000, model="claude-sonnet-5")])
    t = SessionTable()
    s = t.ingest(_event("Stop", transcript_path=str(path)))
    assert s.model == "claude-sonnet-5"
    assert t.to_ble_payload()["ss"][0][4] == 2      # BLE_MODEL_CODES["sonnet"]


def test_read_context_usage_tolerates_junk(tmp_path):
    path = tmp_path / "t.jsonl"
    path.write_text('not json\n{"type":"user"}\n{broken\n', encoding="utf-8")
    assert read_context_usage(path) == (None, None, "")


def test_read_context_usage_missing_file(tmp_path):
    assert read_context_usage(tmp_path / "absent.jsonl") == (None, None, "")


def test_context_pct_from_ingest(tmp_path):
    path = _write_transcript(tmp_path, [_assistant_rec(100_000)])
    t = SessionTable()
    s = t.ingest(_event("Stop", transcript_path=str(path)))
    assert s.context_tokens == 100_000
    assert s.context_pct == 50


def test_fresh_session_reports_zero_not_unknown(tmp_path):
    """A brand-new session genuinely has 0 context; reporting -1 would hide the
    bar as if the transcript were unreadable."""
    empty = tmp_path / "fresh.jsonl"
    empty.write_text("", encoding="utf-8")
    t = SessionTable()
    s = t.ingest(_event("SessionStart", source="startup",
                        transcript_path=str(empty)))
    assert s.context_tokens == 0
    assert s.context_pct == 0


def test_resumed_session_with_unreadable_transcript_stays_unknown(tmp_path):
    """On resume there ARE turns on disk, so a failed read is a genuine unknown
    and must not be reported as 0% used."""
    t = SessionTable()
    s = t.ingest(_event("SessionStart", source="resume",
                        transcript_path=str(tmp_path / "gone.jsonl")))
    assert s.context_tokens is None
    assert s.context_pct is None


def test_stop_with_unreadable_transcript_stays_unknown(tmp_path):
    t = SessionTable()
    s = t.ingest(_event("Stop", transcript_path=str(tmp_path / "gone.jsonl")))
    assert s.context_pct is None


@pytest.mark.parametrize("model,tokens,expected", [
    ("claude-opus-5", 0, 200_000),
    ("claude-opus-5[1m]", 0, 1_000_000),
    ("", 0, 200_000),
    ("claude-opus-5", 50_000, 200_000),
    # Above 200k the window must be bigger, whatever the name says.
    ("claude-opus-5", 440_000, 1_000_000),
    ("", 900_000, 1_000_000),
])
def test_infer_context_limit(model, tokens, expected):
    assert infer_context_limit(model, tokens) == expected


@pytest.mark.parametrize("model,fam", [
    ("claude-opus-5", "opus"),
    ("claude-sonnet-5", "sonnet"),
    ("claude-haiku-4-5-20251001", "haiku"),
    ("claude-fable-5", "fable"),
    ("something-else", ""),
])
def test_model_family(model, fam):
    assert model_family(model) == fam


# ---------------------------------------------------------------------------
# BLE fragment
# ---------------------------------------------------------------------------

def test_ble_payload_shape():
    t = SessionTable()
    path_free = _event("Stop", session_id="a", cwd="/p/netmap")
    t.ingest(path_free)
    frag = t.to_ble_payload()
    assert set(frag) == {"ss", "sn"}
    assert frag["sn"] == 1
    label, state, pct, elapsed, model = frag["ss"][0]
    assert label == "netmap"
    assert pct == -1                    # unknown, no transcript
    assert isinstance(elapsed, int)


def test_ble_payload_reports_true_total_when_truncated():
    """'X more running' depends on sn being the real count, not the rows sent."""
    t = SessionTable()
    for i in range(12):
        t.ingest(_event("Stop", session_id=f"s{i}", cwd=f"/p/project{i}"))
    frag = t.to_ble_payload()
    assert len(frag["ss"]) <= BLE_MAX_ROWS
    assert frag["sn"] == 12


def _maxed_table(rows=BLE_MAX_ROWS, name_len=40):
    """Table with every field at its maximum, for worst-case sizing."""
    t = SessionTable()
    for i in range(rows):
        s = t.ingest(_event("Stop", session_id=f"s{i}",
                            cwd=f"/p/{'w' * name_len}{i}"))
        s.context_tokens = 200_000
        s.context_limit = 200_000
        s.state_since = 0.0                       # forces elapsed to clamp at 65535
        s.model = "claude-sonnet-5"
    return t


def test_ble_payload_worst_case_within_budget():
    """The regression guard that matters: a write-without-response exceeding MTU
    is dropped silently, so an over-budget fragment would blank the screen with
    no error anywhere."""
    frag = _maxed_table().to_ble_payload()
    size = ble_fragment_size(frag)
    assert size <= BLE_ROWS_BUDGET_BYTES, f"{size} bytes exceeds budget"
    assert all(len(r[0]) <= BLE_LABEL_CHARS for r in frag["ss"])
    # Labels shrink to fit; rows must never be sacrificed for them.
    assert len(frag["ss"]) == BLE_MAX_ROWS


def test_ble_payload_uses_full_labels_at_measured_mtu():
    """Measured ATT_MTU 256 on Windows -> 243-byte budget -> full 20-char labels."""
    frag = _maxed_table().to_ble_payload(max_bytes=budget_from_mtu(256))
    assert len(frag["ss"]) == BLE_MAX_ROWS
    assert all(len(r[0]) == BLE_LABEL_CHARS for r in frag["ss"])
    assert ble_fragment_size(frag) <= budget_from_mtu(256)


def test_ble_payload_shrinks_labels_before_dropping_rows():
    """Hiding a session is the one thing this screen must not do, so a tight
    budget costs label characters, not whole rows."""
    # 5 rows cost 216 B at the 20-char cap and 156 B at the 8-char floor, so a
    # budget in between must be met by shrinking rather than dropping.
    frag = _maxed_table().to_ble_payload(max_bytes=180)
    assert ble_fragment_size(frag) <= 180
    assert len(frag["ss"]) == BLE_MAX_ROWS, "rows dropped before labels shrank"
    assert all(len(r[0]) < BLE_LABEL_CHARS for r in frag["ss"])


def test_ble_payload_degrades_monotonically():
    """Property rather than magic numbers: as the budget shrinks, labels shorten
    before rows go, rows never increase, and the floor is respected throughout.

    Deliberately not asserting an exact byte threshold for the 5-row floor -- that
    figure moves with incidental field widths (state 1 vs 10, sn 5 vs 99), which
    would make the test brittle without testing anything real.
    """
    prev_rows, prev_chars = BLE_MAX_ROWS + 1, BLE_LABEL_CHARS + 1
    for budget in range(260, 59, -10):
        frag = _maxed_table().to_ble_payload(max_bytes=budget)
        rows = frag["ss"]
        assert ble_fragment_size(frag) <= budget, f"over budget at {budget}"
        assert len(rows) <= prev_rows, f"row count grew as budget shrank at {budget}"
        if rows:
            chars = max(len(r[0]) for r in rows)
            assert chars <= prev_chars, f"labels grew as budget shrank at {budget}"
            assert chars <= BLE_LABEL_CHARS
            # Below the floor only ever happens via the cap, never the shrink loop.
            assert chars >= BLE_LABEL_CHARS_MIN or len(rows) < BLE_MAX_ROWS
            prev_chars = chars
        prev_rows = len(rows)
    assert frag["sn"] == BLE_MAX_ROWS, "true total must survive every truncation"


def test_ble_payload_drops_rows_only_at_the_label_floor():
    frag = _maxed_table().to_ble_payload(max_bytes=80)
    assert ble_fragment_size(frag) <= 80
    assert 0 < len(frag["ss"]) < BLE_MAX_ROWS
    assert frag["sn"] == BLE_MAX_ROWS          # total still truthful
    assert all(len(r[0]) <= BLE_LABEL_CHARS_MIN for r in frag["ss"])


@pytest.mark.parametrize("mtu,expected", [
    (256, 243),                      # measured on Windows/WinRT
    (517, 504),
    (23, BLE_ROWS_BUDGET_BYTES),     # BLE default = backend guessing, not truth
    (None, BLE_ROWS_BUDGET_BYTES),
    (0, BLE_ROWS_BUDGET_BYTES),
    (100, BLE_ROWS_BUDGET_BYTES),    # never go BELOW the conservative default
])
def test_budget_from_mtu(mtu, expected):
    assert budget_from_mtu(mtu) == expected


@pytest.mark.parametrize("name,chars,expected", [
    ("netmap", 20, "netmap"),                       # fits, untouched
    ("data-pipeline-svc", 20, "data-pipeline-svc"), # longest real name, fits
    ("a-really-long-project", 20, "a-really-long-pro..."),  # no discriminator
    ("exactly-twenty-chars", 20, "exactly-twenty-chars"),
    ("abcdef", 6, "abcdef"),
    ("abcdefg", 6, "abc..."),
    ("abcdefg", 3, "abc"),                          # no room for dots
])
def test_ble_label_marks_truncation_within_the_cap(name, chars, expected):
    """Dots are counted inside the cap, so signalling truncation is free."""
    out = ble_label(name, chars)
    assert out == expected
    assert len(out) <= chars


@pytest.mark.parametrize("name,chars,expected", [
    ("somelongprojectname-12", 20, "somelongprojec...-12"),
    ("netmap-57", 20, "netmap-57"),                 # fits, untouched
    ("data-pipeline-svc-3", 20, "data-pipeline-svc-3"),
    ("frontend-tests-127", 16, "frontend-...-127"),
    ("a-very-long-directory-name-999", 20, "a-very-long-d...-999"),
    # Observed on hardware: discriminators are NOT always numeric. A `-\\d+$`
    # pattern silently failed to protect these.
    ("somelongprojectname-2c", 20, "somelongprojec...-2c"),
    ("a-very-long-directory-name-a1b", 20, "a-very-long-d...-a1b"),
])
def test_ble_label_preserves_the_session_discriminator(name, chars, expected):
    """The suffix is the only thing distinguishing two sessions in one directory,
    so truncation elides the middle rather than the tail."""
    out = ble_label(name, chars, keep_suffix=True)
    assert out == expected
    assert len(out) <= chars
    assert out.split("-")[-1] == name.split("-")[-1], "discriminator lost"


def test_ble_label_does_not_elide_plain_project_names():
    """Without a roster the label is basename(cwd), where a trailing word is part
    of the name, not a discriminator -- middle-eliding it would be wrong."""
    assert ble_label("data-pipeline-svc-extra", 20) == "data-pipeline-svc..."


def test_alphanumeric_discriminator_survives_truncation_end_to_end():
    t = SessionTable()
    s = t.ingest(_event("Stop", cwd="/home/user/a-very-long-project-dir"))
    s.derived_name = "a-very-long-project-dir-2c"
    row = t.to_ble_payload()["ss"][0]
    assert row[0].endswith("-2c"), f"discriminator lost: {row[0]!r}"
    assert len(row[0]) <= BLE_LABEL_CHARS


def test_ble_label_falls_back_when_suffix_eats_the_budget():
    """A discriminator so long there is no usable stem left reads better as a
    plain truncation than as '...-1234567'."""
    out = ble_label("ab-1234567890", 12, keep_suffix=True)
    assert out == "ab-123456..."
    assert len(out) <= 12


def test_two_sessions_in_one_directory_are_distinguishable():
    """The defect the roster exists to fix: basename(cwd) alone collides."""
    t = SessionTable()
    a = t.ingest(_event("Stop", session_id="a", cwd="/home/user/netmap"))
    b = t.ingest(_event("Stop", session_id="b", cwd="/home/user/netmap"))
    assert a.label == b.label == "netmap"      # without a roster: ambiguous
    a.derived_name, b.derived_name = "netmap-57", "netmap-12"
    assert a.label != b.label
    frag = t.to_ble_payload()
    labels = [r[0] for r in frag["ss"]]
    assert sorted(labels) == ["netmap-12", "netmap-57"]


def test_ble_label_stays_ascii():
    """U+2026 would cost 3 UTF-8 bytes AND miss the Styrene fonts, which only
    cover ASCII 32-126."""
    out = ble_label("a-really-long-project-name", 20)
    assert out.isascii()
    assert "…" not in out
    assert len(out.encode()) == len(out)


def test_ble_payload_empty_when_no_sessions():
    frag = SessionTable().to_ble_payload()
    assert frag == {"ss": [], "sn": 0}


def test_ble_payload_prefers_attention_rows_under_truncation():
    t = SessionTable()
    for i in range(8):
        t.ingest(_event("Stop", session_id=f"s{i}", cwd=f"/p/project{i}"))
    t.ingest(_event("PreToolUse", session_id="s7", cwd="/p/project7",
                    tool_name="AskUserQuestion"))
    frag = t.to_ble_payload(max_bytes=60)
    assert frag["ss"], "at least one row must survive"
    assert frag["ss"][0][0] == "project7"


# ---------------------------------------------------------------------------
# HTTP endpoint
# ---------------------------------------------------------------------------

async def _post(port, body: bytes, path="/hook", headers=True):
    reader, writer = await asyncio.open_connection("127.0.0.1", port)
    if headers:
        req = (
            f"POST {path} HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\n"
            "Content-Type: application/json\r\n"
            f"Content-Length: {len(body)}\r\n\r\n"
        ).encode() + body
    else:
        req = body
    writer.write(req)
    await writer.drain()
    resp = await asyncio.wait_for(reader.read(-1), 5)
    writer.close()
    return resp.decode("latin-1", errors="replace")


def test_listener_disabled_when_port_none():
    async def go():
        listener = HookListener(None)
        assert await listener.start() is False
        assert listener.running is False
    _run(go())


def test_http_end_to_end_updates_table():
    async def go():
        # port 0 = let the OS pick, so the suite never clashes with a real daemon
        async with HookListener(0) as listener:
            port = listener.bound_port
            body = json.dumps(_event("PermissionRequest", tool_name="Bash")).encode()
            resp = await _post(port, body)
            assert "204" in resp.split("\r\n")[0]
            s = listener.table.get("s1")
            assert s is not None
            assert s.state is SessionState.WAITING_PERMISSION
            assert listener.updated.is_set()
    _run(go())


def test_http_multiple_sessions_tracked_independently():
    async def go():
        async with HookListener(0) as listener:
            port = listener.bound_port
            await _post(port, json.dumps(
                _event("Stop", session_id="a", cwd="/p/one")).encode())
            await _post(port, json.dumps(
                _event("PreToolUse", session_id="b", cwd="/p/two",
                       tool_name="AskUserQuestion")).encode())
            assert listener.table.get("a").state is SessionState.IDLE
            assert listener.table.get("b").state is SessionState.WAITING_QUESTION
            assert listener.table.events_received == 2
    _run(go())


def test_http_rejects_malformed_json():
    async def go():
        async with HookListener(0) as listener:
            port = listener.bound_port
            resp = await _post(port, b"{not json")
            assert "400" in resp.split("\r\n")[0]
            assert len(listener.table) == 0
    _run(go())


def test_http_no_body_is_accepted():
    """A health-check GET must not 500."""
    async def go():
        async with HookListener(0) as listener:
            port = listener.bound_port
            reader, writer = await asyncio.open_connection("127.0.0.1", port)
            writer.write(b"GET /hook HTTP/1.1\r\nHost: x\r\n\r\n")
            await writer.drain()
            resp = (await asyncio.wait_for(reader.read(-1), 5)).decode("latin-1")
            writer.close()
            assert "204" in resp.split("\r\n")[0]
    _run(go())


def test_http_truncated_request_does_not_raise():
    """readuntil/readexactly signal truncation with non-OSError exceptions; if
    they escaped they would surface as unhandled asyncio task errors."""
    async def go():
        async with HookListener(0) as listener:
            port = listener.bound_port
            reader, writer = await asyncio.open_connection("127.0.0.1", port)
            # Promise 500 bytes, send 5, then hang up.
            writer.write(b"POST /hook HTTP/1.1\r\nContent-Length: 500\r\n\r\nabcde")
            await writer.drain()
            writer.close()
            try:
                await writer.wait_closed()
            except OSError:
                pass
            # Listener must still serve the next request.
            resp = await _post(port, json.dumps(_event("Stop")).encode())
            assert "204" in resp.split("\r\n")[0]
    _run(go())


def test_http_oversized_content_length_rejected():
    async def go():
        async with HookListener(0) as listener:
            port = listener.bound_port
            reader, writer = await asyncio.open_connection("127.0.0.1", port)
            writer.write(b"POST /hook HTTP/1.1\r\nContent-Length: 99999999\r\n\r\n")
            await writer.drain()
            resp = (await asyncio.wait_for(reader.read(-1), 5)).decode("latin-1")
            writer.close()
            assert "400" in resp.split("\r\n")[0]
    _run(go())


def test_http_bind_failure_is_not_fatal():
    """A port clash must leave the daemon doing its existing job."""
    async def go():
        async with HookListener(0) as first:
            port = first.bound_port
            second = HookListener(port)
            assert await second.start() is False
            assert second.running is False
    _run(go())


# ---------------------------------------------------------------------------
# Roster (~/.claude/sessions)
# ---------------------------------------------------------------------------

def _write_roster(tmp_path, entries):
    """Build a fake ~/.claude/sessions directory. Uses this process's own PID so
    the liveness check passes."""
    d = tmp_path / "sessions"
    d.mkdir(parents=True, exist_ok=True)
    for i, e in enumerate(entries):
        rec = {"pid": e.get("pid", os.getpid()), "sessionId": e["sessionId"],
               "cwd": e.get("cwd", "/p/x"), "name": e.get("name", ""),
               "kind": "interactive", "version": "2.1.220"}
        (d / f"{rec['pid'] + i}.json").write_text(json.dumps(rec), encoding="utf-8")
    return tmp_path


def test_scan_live_sessions_reads_names(tmp_path):
    base = _write_roster(tmp_path, [{"sessionId": "abc", "name": "netmap-57"}])
    roster = scan_live_sessions([base])
    assert roster is not None
    assert roster["abc"]["name"] == "netmap-57"
    assert roster["abc"]["kind"] == "interactive"


def test_scan_live_sessions_returns_none_when_unreadable(tmp_path):
    """None means 'unknown', not 'nothing running' -- a missing directory must
    never be allowed to wipe the display."""
    assert scan_live_sessions([tmp_path / "absent"]) is None


def test_scan_live_sessions_skips_dead_pids(tmp_path):
    """A hard crash leaves the file behind; the PID check is what catches it."""
    base = _write_roster(tmp_path, [
        {"sessionId": "alive", "name": "a-1"},
        {"sessionId": "dead", "name": "d-1", "pid": 999_999_998},
    ])
    roster = scan_live_sessions([base])
    assert "alive" in roster
    assert "dead" not in roster


def test_roster_supplies_derived_name(tmp_path):
    base = _write_roster(tmp_path, [{"sessionId": "s1", "name": "netmap-57"}])
    t = SessionTable(config_dirs=[base])
    s = t.ingest(_event("Stop"))
    assert s.derived_name == "netmap-57"
    assert s.label == "netmap-57"


def test_missing_from_roster_retires_the_session(tmp_path):
    """Answers the idle-vs-crashed question: liveness comes from the roster, so a
    vanished process is retired promptly instead of squatting for 6 hours."""
    base = _write_roster(tmp_path, [{"sessionId": "s1", "name": "netmap-57"}])
    t = SessionTable(config_dirs=[base])
    s = t.ingest(_event("Stop"))
    assert s.state is SessionState.IDLE
    assert len(t.active()) == 1

    # Process disappears without a SessionEnd, and the grace period has passed.
    for f in (base / "sessions").glob("*.json"):
        f.unlink()
    s.started_at -= 3600
    t.refresh_roster(force=True)
    assert t.active() == []
    assert s.state is SessionState.ENDED
    assert s.end_reason == "process gone"


def test_roster_grace_protects_a_brand_new_session(tmp_path):
    """A hook can arrive before its roster file is visible; that must not read as
    a dead process."""
    base = tmp_path
    (base / "sessions").mkdir()
    t = SessionTable(config_dirs=[base])
    s = t.ingest(_event("Stop"))
    assert len(t.active()) == 1, "new session retired inside the grace window"
    assert s.state is SessionState.IDLE


def test_unreadable_roster_leaves_sessions_alone(tmp_path):
    t = SessionTable(config_dirs=[tmp_path / "absent"])
    t.ingest(_event("Stop"))
    assert len(t.active()) == 1
    assert t.refresh_roster(force=True) is None


def test_pid_alive_on_self_and_absurd_pid():
    assert _pid_alive(os.getpid()) is True
    assert _pid_alive(0) is False
    assert _pid_alive(-1) is False


def test_snapshot_is_json_serialisable():
    """Diagnostics must survive json.dumps for logging."""
    t = SessionTable()
    t.ingest(_event("Stop", last_assistant_message="hello"))
    json.dumps(t.snapshot())
