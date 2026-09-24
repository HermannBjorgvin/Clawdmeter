"""Test isolation for the daemon suite.

The poll path gained a second provider, and a collector that reads real
credentials and makes a real request. Left alone, `pytest` on a machine with
Codex signed in performs live HTTP and returns that machine's usage numbers --
so results differ between developer laptops and CI, and an exact-payload
assertion fails for reasons unrelated to what it tests.

Default every test to "Codex unavailable". Tests that care about Codex
override this explicitly.
"""
import pytest


@pytest.fixture(autouse=True)
def _no_live_codex(monkeypatch):
    try:
        import daemon.claude_usage_daemon as mod
    except ImportError:          # suites that never import the macOS daemon
        return
    monkeypatch.setattr(mod._CODEX, "collect_blocking", lambda: None,
                        raising=False)
