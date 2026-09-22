#!/usr/bin/env python3
"""Tests for the Codex usage collector.

The two bugs these guard against were both found on real data:

  * Reading the newest rollout record blindly returned 0% off a per-model
    bucket while the account was actually at 78%.
  * primary/secondary are positional, not semantic -- a Pro plan puts its
    7-day window in `primary`, a per-model block puts 5h there.

Run: python -m pytest daemon/tests/test_codex_collector.py -x -q
"""
import asyncio
import json
import urllib.error
from unittest.mock import patch

from daemon.collectors import WINDOW_5H, WINDOW_7D
from daemon.collectors.codex import (
    CodexCollector,
    _windows_from_endpoint,
    collect_via_logs,
    collect_via_oauth,
)


# --- endpoint shape ---------------------------------------------------------

def test_endpoint_windows_keyed_by_duration_not_position():
    """A Pro plan's 7-day window arrives in `primary`, with secondary null."""
    windows = _windows_from_endpoint({
        "primary_window": {"used_percent": 78, "limit_window_seconds": 604800,
                           "reset_after_seconds": 428901},
        "secondary_window": None,
    })
    assert set(windows) == {WINDOW_7D}
    assert windows[WINDOW_7D].used_percent == 78
    assert windows[WINDOW_7D].resets_in == 428901


def test_endpoint_windows_handles_both_slots():
    """A per-model block uses primary for 5h and secondary for 7d."""
    windows = _windows_from_endpoint({
        "primary_window": {"used_percent": 12, "limit_window_seconds": 18000},
        "secondary_window": {"used_percent": 3, "limit_window_seconds": 604800},
    })
    assert windows[WINDOW_5H].used_percent == 12
    assert windows[WINDOW_7D].used_percent == 3


def test_unknown_window_length_is_dropped_not_guessed():
    assert _windows_from_endpoint(
        {"primary_window": {"used_percent": 5, "limit_window_seconds": 999}}
    ) == {}


# --- free-ride credential rule ----------------------------------------------

def _auth_dir(tmp_path):
    (tmp_path / "auth.json").write_text(json.dumps(
        {"tokens": {"access_token": "tok", "account_id": "acct"}}))
    return tmp_path


def test_dead_token_reports_no_data_and_never_refreshes(tmp_path):
    """401 means "no data", not a refresh -- the Codex CLI owns the token."""
    err = urllib.error.HTTPError(url="", code=401, msg="", hdrs=None, fp=None)
    with patch("urllib.request.urlopen", side_effect=err):
        assert collect_via_oauth(_auth_dir(tmp_path)) is None


def test_signed_out_reports_no_data(tmp_path):
    """No auth.json at all."""
    assert collect_via_oauth(tmp_path) is None


# --- rollout-log fallback ---------------------------------------------------

def _rollout(tmp_path, *records):
    d = tmp_path / "sessions" / "2026" / "08" / "20"
    d.mkdir(parents=True)
    path = d / "rollout-2026-08-20T10-00-12-test.jsonl"
    path.write_text("\n".join(
        json.dumps({"payload": {"rate_limits": r}}) for r in records))
    return path


PER_MODEL = {
    "limit_id": "codex_bengalfox", "limit_name": "GPT-5.3-Codex-Spark",
    "primary": {"used_percent": 0.0, "window_minutes": 300, "resets_at": 0},
    "secondary": {"used_percent": 0.0, "window_minutes": 10080, "resets_at": 0},
}
ACCOUNT = {
    "limit_id": "codex", "plan_type": "pro",
    "primary": {"used_percent": 78.0, "window_minutes": 10080, "resets_at": 0},
    "secondary": None,
}


def test_per_model_bucket_never_masquerades_as_the_account_limit(tmp_path):
    """The account record is older, so the naive "newest wins" read got 0%."""
    _rollout(tmp_path, ACCOUNT, PER_MODEL)
    snap = collect_via_logs(tmp_path)
    assert snap is not None
    assert snap.windows[WINDOW_7D].used_percent == 78.0
    assert WINDOW_5H not in snap.windows  # 5h belonged to the per-model bucket


def test_no_account_record_reports_no_data(tmp_path):
    """Per-model records alone are not a usable answer."""
    _rollout(tmp_path, PER_MODEL)
    assert collect_via_logs(tmp_path) is None


def test_log_snapshot_is_never_reported_as_live(tmp_path):
    """A log file written a second ago is still not an on-demand read."""
    _rollout(tmp_path, ACCOUNT)
    snap = collect_via_logs(tmp_path)
    assert snap.source.startswith("logs:")
    assert not snap.live


# --- fallback ordering ------------------------------------------------------

def _run(coro):
    """Run a coroutine from a sync test on its own event loop."""
    return asyncio.run(coro)


def test_logs_used_only_when_the_endpoint_fails(tmp_path):
    _auth_dir(tmp_path)
    _rollout(tmp_path, ACCOUNT)
    with patch("urllib.request.urlopen", side_effect=urllib.error.URLError("offline")):
        snap = _run(CodexCollector(tmp_path).collect())
    assert snap.source.startswith("logs:")


def test_endpoint_preferred_over_logs(tmp_path):
    """A live read wins even when a (necessarily staler) log record exists."""
    _auth_dir(tmp_path)
    _rollout(tmp_path, ACCOUNT)

    class _Resp:
        status = 200
        def read(self):
            return json.dumps({"plan_type": "pro", "rate_limit": {
                "primary_window": {"used_percent": 42,
                                   "limit_window_seconds": 604800,
                                   "reset_after_seconds": 100}}}).encode()
        def __enter__(self): return self
        def __exit__(self, *a): return False

    with patch("urllib.request.urlopen", return_value=_Resp()):
        snap = _run(CodexCollector(tmp_path).collect())
    assert snap.live and snap.windows[WINDOW_7D].used_percent == 42


# --- wire payload for the device -------------------------------------------

def _snap(**windows):
    from daemon.collectors import UsageSnapshot
    return UsageSnapshot(provider="codex", plan="pro", windows=windows)


def test_absent_window_is_reported_absent_not_zero(monkeypatch):
    """A Codex Pro plan has no 5h quota. 0% would be a lie about a real limit."""
    from daemon.collectors import Window
    import daemon.claude_usage_daemon as mod

    monkeypatch.setattr(mod._CODEX, "collect_blocking",
                        lambda: _snap(**{WINDOW_7D: Window(83.0, resets_in=6891 * 60)}))
    p = mod.codex_payload()
    # The lone quota takes the prominent panel (see the single-quota test);
    # what matters here is that the slot WITHOUT a quota is marked absent
    # rather than reported as 0%.
    assert (p["s"], p["sr"], p["has_s"]) == (83, 6891, True)
    assert p["has_w"] is False


def test_both_windows_map_through(monkeypatch):
    from daemon.collectors import Window
    import daemon.claude_usage_daemon as mod

    monkeypatch.setattr(mod._CODEX, "collect_blocking",
                        lambda: _snap(**{WINDOW_5H: Window(12.0, resets_in=3600),
                                         WINDOW_7D: Window(40.0, resets_in=86400)}))
    p = mod.codex_payload()
    assert (p["s"], p["sr"], p["has_s"]) == (12, 60, True)
    assert (p["w"], p["wr"], p["has_w"]) == (40, 1440, True)


def test_no_codex_yields_no_payload(monkeypatch):
    import daemon.claude_usage_daemon as mod
    monkeypatch.setattr(mod._CODEX, "collect_blocking", lambda: None)
    assert mod.codex_payload() is None


def test_poll_active_itself_merges_codex(monkeypatch):
    """Regression: the merge must live where the run loop actually calls.

    It was first written into poll_active_payload, which reads like the entry
    point but is not one -- run() calls poll_active() directly because it needs
    the all-dead flag. Every unit test passed while the device never once
    received an "x" key. Assert against poll_active, not the wrapper.
    """
    import asyncio
    from pathlib import Path
    import daemon.claude_usage_daemon as mod

    monkeypatch.setattr(mod, "read_config_dirs", lambda: [Path("/fake")])
    monkeypatch.setattr(mod, "read_token_for", lambda d: "tok")

    async def fake_poll_api(_token):
        return {"s": 25, "ok": True}
    monkeypatch.setattr(mod, "poll_api", fake_poll_api)
    monkeypatch.setattr(mod, "codex_payload", lambda: {"w": 83, "has_w": True})

    payload, dead = asyncio.run(mod.poll_active(mod.PlanSelector()))
    assert dead is False
    assert payload["s"] == 25            # Claude still at the top level
    assert payload["x"] == {"w": 83, "has_w": True}


def test_model_weekly_on_top_account_weekly_below(monkeypatch):
    """Codex Pro has no account 5h window, so the panels are both weekly.

    Top carries the model's weekly, bottom the account's -- and the pills name
    the scope, since being weekly is the one thing they have in common. Both
    must be shown: Spark can sit at 0% while the account weekly is nearly
    spent, and it is the account limit that actually stops you.
    """
    from daemon.collectors import UsageSnapshot, Window
    import daemon.claude_usage_daemon as mod

    snap = UsageSnapshot(
        provider="codex", plan="pro",
        windows={WINDOW_7D: Window(84.0, resets_in=6858 * 60)},
        model_windows={"GPT-5.3-Codex-Spark": {
            WINDOW_5H: Window(3.0, resets_in=300 * 60),
            WINDOW_7D: Window(0.0, resets_in=10080 * 60)}})
    monkeypatch.setattr(mod._CODEX, "collect_blocking", lambda: snap)

    p = mod.codex_payload()
    assert (p["s"], p["sr"], p["sm"]) == (0, 10080, "Spark")
    assert (p["w"], p["wr"], p["wm"]) == (84, 6858, "Overall")


def test_without_a_model_weekly_it_falls_back_to_plain_windows(monkeypatch):
    """No model weekly to pair with: 5h on top, weekly below, unlabelled."""
    from daemon.collectors import UsageSnapshot, Window
    import daemon.claude_usage_daemon as mod

    snap = UsageSnapshot(
        provider="codex", plan="pro",
        windows={WINDOW_5H: Window(30.0, resets_in=600),
                 WINDOW_7D: Window(50.0, resets_in=6000)})
    monkeypatch.setattr(mod._CODEX, "collect_blocking", lambda: snap)

    p = mod.codex_payload()
    assert (p["s"], p["w"]) == (30, 50)
    assert "sm" not in p and "wm" not in p



def test_account_window_wins_over_a_model_window(monkeypatch):
    """A model bucket is a fallback, never a replacement."""
    from daemon.collectors import UsageSnapshot, Window
    import daemon.claude_usage_daemon as mod

    snap = UsageSnapshot(
        provider="codex", plan="pro",
        windows={WINDOW_5H: Window(30.0, resets_in=600)},
        model_windows={"GPT-5.3-Codex-Spark": {WINDOW_5H: Window(99.0, resets_in=60)}})
    monkeypatch.setattr(mod._CODEX, "collect_blocking", lambda: snap)

    p = mod.codex_payload()
    assert p["s"] == 30 and "sm" not in p


def test_codex_survives_a_dead_claude_token(monkeypatch):
    """A 401 on Claude says nothing about Codex, and used to hide it anyway.

    The device reads Claude's fields from the top level, so the no-data beat
    still carries ok:false there; a readable provider rides along under "x".
    """
    import daemon.claude_usage_daemon as mod
    monkeypatch.setattr(mod._CODEX, "collect_blocking",
                        lambda: _snap(**{WINDOW_7D: __import__(
                            "daemon.collectors", fromlist=["Window"]).Window(
                                85.0, resets_in=5722 * 60)}))
    codex = mod.codex_payload()
    beat = {"ok": False}
    if codex:
        beat["x"] = codex
    assert beat["ok"] is False          # Claude mode still says "No data"
    assert beat["x"]["s"] == 85         # Codex mode stays live (lone quota, top slot)


def test_expired_log_window_is_not_carried_forward(tmp_path):
    """A record whose window already reset describes a different period.

    Observed live: the endpoint was timing out, the fallback served a record
    from before the weekly rollover, and the device showed 69% of a spent week
    while the account was actually 10% into a fresh one.
    """
    import time
    past = {
        "limit_id": "codex", "plan_type": "pro",
        "primary": {"used_percent": 69.0, "window_minutes": 10080,
                    "resets_at": int(time.time()) - 60},   # already reset
        "secondary": None,
    }
    _rollout(tmp_path, past)
    assert collect_via_logs(tmp_path) is None


def test_live_log_window_still_used(tmp_path):
    """The guard must not reject a record whose window is genuinely open."""
    import time
    live = {
        "limit_id": "codex", "plan_type": "pro",
        "primary": {"used_percent": 22.0, "window_minutes": 10080,
                    "resets_at": int(time.time()) + 3600},
        "secondary": None,
    }
    _rollout(tmp_path, live)
    snap = collect_via_logs(tmp_path)
    assert snap is not None and snap.windows[WINDOW_7D].used_percent == 22.0


def test_stale_log_record_is_refused(tmp_path):
    """16-hour-old records reported 69% while the live endpoint said 10%.

    The fallback rides out a brief outage; it does not keep yesterday's number
    on screen. A blank panel is recoverable, a confidently wrong one is not.
    """
    import os, time
    from daemon.collectors.codex import MAX_LOG_AGE_S
    path = _rollout(tmp_path, ACCOUNT)
    old = time.time() - (MAX_LOG_AGE_S + 600)
    os.utime(path, (old, old))
    assert collect_via_logs(tmp_path) is None


def test_single_quota_is_promoted_to_the_prominent_panel(monkeypatch):
    """OpenAI dropped additional_rate_limits in Sept 2026.

    Pro accounts then report one weekly window and nothing else, which under
    the old 5h-on-top mapping put a dash in the prominent panel and the only
    real number underneath it. The lone quota now takes the top slot and the
    second is marked absent so the device drops the empty card.
    """
    from daemon.collectors import UsageSnapshot, Window
    import daemon.claude_usage_daemon as mod

    snap = UsageSnapshot(provider="codex", plan="pro",
                         windows={WINDOW_7D: Window(26.0, resets_in=434649)})
    monkeypatch.setattr(mod._CODEX, "collect_blocking", lambda: snap)

    p = mod.codex_payload()
    assert (p["s"], p["has_s"], p["sm"]) == (26, True, "Weekly")
    assert p["has_w"] is False          # device hides the second card


def test_reset_credits_ride_along_with_count_and_expiry(monkeypatch):
    """The second card carries reset credits when there is no second quota.

    wham/usage reports only how many credits exist; the expiry comes from the
    detail endpoint, so the payload carries both a count and minutes until
    the soonest expiry -- a duration, like sr/wr, because the device has no
    date handling and formats every countdown the same way.
    """
    import time
    from daemon.collectors import UsageSnapshot, Window
    import daemon.claude_usage_daemon as mod

    expiry = time.time() + 11 * 1440 * 60 + 3 * 3600 + 30   # 11d 3h from now
    snap = UsageSnapshot(provider="codex", plan="pro",
                         windows={WINDOW_7D: Window(31.0, resets_in=7177 * 60)},
                         reset_credits=2, reset_credits_expire=expiry)
    monkeypatch.setattr(mod._CODEX, "collect_blocking", lambda: snap)

    p = mod.codex_payload()
    assert p["rc"] == 2 and p["rm"] == 11 * 1440 + 3 * 60

    # A lapsed stamp the endpoint has not pruned yet clamps at 0, never negative.
    import dataclasses
    lapsed = dataclasses.replace(snap, reset_credits_expire=time.time() - 600)
    monkeypatch.setattr(mod._CODEX, "collect_blocking", lambda: lapsed)
    assert mod.codex_payload()["rm"] == 0


def test_no_credits_means_no_keys(monkeypatch):
    """A provider without reset credits sends nothing, so the card stays gone."""
    from daemon.collectors import UsageSnapshot, Window
    import daemon.claude_usage_daemon as mod

    snap = UsageSnapshot(provider="codex", plan="pro",
                         windows={WINDOW_7D: Window(31.0, resets_in=600)})
    monkeypatch.setattr(mod._CODEX, "collect_blocking", lambda: snap)

    p = mod.codex_payload()
    assert "rc" not in p and "rm" not in p


# --- reset-credit lifetimes -------------------------------------------------

def test_credit_life_is_percent_of_granted_lifetime_left():
    """The card draws each credit draining, so the collector reports life left.

    A 30-day grant a quarter of the way through has three quarters of its life
    left -- not "75% used", the inverse of the quota bars above it.
    """
    from daemon.collectors.codex import _credit_life

    day = 86400.0
    now = 1_000_000.0
    # granted 10 days ago, expires in 20 -> 2/3 of a 30-day life left.
    assert _credit_life(((now - 10 * day, now + 20 * day),), now) == (67,)
    # Already lapsed, and a degenerate zero-length span: both clamp rather
    # than going negative or dividing by zero.
    assert _credit_life(((now - 30 * day, now - day),), now) == (0,)
    assert _credit_life(((now, now),), now) == (100,)


def test_credit_stamps_are_sorted_soonest_expiry_first(tmp_path, monkeypatch):
    """Leftmost pip is the next credit to lapse, so order is not incidental."""
    import daemon.collectors.codex as cx

    payload = {"credits": [
        {"status": "available", "granted_at": "2026-09-05T04:19:53.865558Z",
         "expires_at": "2026-10-05T04:19:53.865558Z"},
        {"status": "available", "granted_at": "2026-09-04T02:29:58.238421Z",
         "expires_at": "2026-10-04T02:29:58.238421Z"},
        {"status": "redeemed", "granted_at": "2026-08-01T00:00:00Z",
         "expires_at": "2026-08-31T00:00:00Z"},   # spent -- not a holding
    ]}
    monkeypatch.setattr(cx, "_credits_cache", {"at": 0.0, "stamps": ()})
    with patch("urllib.request.urlopen") as u:
        u.return_value.__enter__.return_value.read.return_value = \
            json.dumps(payload).encode()
        stamps = cx._credit_stamps("tok", "acct")

    assert len(stamps) == 2                       # the redeemed one is gone
    assert stamps[0][1] < stamps[1][1]            # Oct 4 before Oct 5


def test_cached_credits_still_age(tmp_path, monkeypatch):
    """The cache holds raw stamps, so life left keeps moving between fetches.

    Caching the derived percentage instead would freeze the pips for a whole
    hour at a time -- the one thing the visual is there to show.
    """
    import daemon.collectors.codex as cx

    day = 86400.0
    granted, expires = 1_000_000.0, 1_000_000.0 + 30 * day
    monkeypatch.setattr(cx, "_credits_cache",
                        {"at": 9e18, "stamps": ((granted, expires),)})  # fresh

    with patch("urllib.request.urlopen", side_effect=AssertionError("no HTTP")):
        early = cx._credit_life(cx._credit_stamps("tok", None), granted + day)
        late = cx._credit_life(cx._credit_stamps("tok", None), granted + 29 * day)
    assert early == (97,) and late == (3,)


def test_credit_stamps_keep_last_known_value_when_endpoint_fails(monkeypatch):
    """A slow endpoint must not erase pips that are still genuinely held."""
    import daemon.collectors.codex as cx

    known = ((1.0, 2.0),)
    monkeypatch.setattr(cx, "_credits_cache", {"at": 0.0, "stamps": known})
    with patch("urllib.request.urlopen", side_effect=urllib.error.URLError("x")):
        assert cx._credit_stamps("tok", None) == known


def test_payload_sends_the_head_of_the_queue(monkeypatch):
    """"rl" is the soonest-expiring credit's life left -- the only clock the
    card shows, because it is the one you spend first."""
    from daemon.collectors import UsageSnapshot, Window
    import daemon.claude_usage_daemon as mod

    snap = UsageSnapshot(provider="codex", plan="pro",
                         windows={WINDOW_7D: Window(31.0, resets_in=600)},
                         reset_credits=2, reset_credits_expire=1.0,
                         reset_credit_life=(37, 71))
    monkeypatch.setattr(mod._CODEX, "collect_blocking", lambda: snap)

    assert mod.codex_payload()["rl"] == 37


def test_payload_count_is_never_capped(monkeypatch):
    """However many are held, the number reports the true count."""
    from daemon.collectors import UsageSnapshot, Window
    import daemon.claude_usage_daemon as mod

    snap = UsageSnapshot(provider="codex", plan="pro",
                         windows={WINDOW_7D: Window(31.0, resets_in=600)},
                         reset_credits=9,
                         reset_credit_life=tuple(range(10, 100, 10)))
    monkeypatch.setattr(mod._CODEX, "collect_blocking", lambda: snap)

    p = mod.codex_payload()
    assert p["rc"] == 9 and p["rl"] == 10


def test_unknown_lifetimes_send_no_rl_key(monkeypatch):
    """Detail endpoint down: send the count, and let the device leave the
    track empty rather than inventing a life for the credit."""
    from daemon.collectors import UsageSnapshot, Window
    import daemon.claude_usage_daemon as mod

    snap = UsageSnapshot(provider="codex", plan="pro",
                         windows={WINDOW_7D: Window(31.0, resets_in=600)},
                         reset_credits=2)
    monkeypatch.setattr(mod._CODEX, "collect_blocking", lambda: snap)

    p = mod.codex_payload()
    assert p["rc"] == 2 and "rl" not in p and "rm" not in p
