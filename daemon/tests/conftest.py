"""Shared pytest fixtures for the daemon test suite.

Safety net for the OAuth self-refresh added to the Windows daemon.

`connect_and_run` now (a) reads the live credential store via
`_credentials_expiry_seconds()` to decide whether to refresh proactively, and
(b) on a 401/403 (`AuthError`) calls `refresh_access_token()` — which POSTs the
user's REAL refresh token to platform.claude.com and rotates it in place. The
pre-existing connect_and_run tests mock `read_token` / `poll_api` but not these
two new touchpoints, so without this fixture a developer whose token happened to
be near expiry (or whose poll mock raises AuthError) could have their real token
rotated mid-test, with real network I/O.

This autouse fixture neutralizes BOTH touchpoints at the module-global level —
which is exactly where `connect_and_run` looks them up — so the existing tests
stay hermetic with no edits. Tests that exercise the refresh helpers directly
import them by name (``from ... import refresh_access_token``), so their bindings
point at the originals and are unaffected. Tests that want to drive the wired-up
behavior simply override these inside their own ``patch()`` blocks (an inner
patch wins for the duration of the test body).
"""
import sys
from unittest.mock import AsyncMock

import pytest

_WIN_MODULE = "daemon.claude_usage_daemon_windows"


@pytest.fixture(autouse=True)
def _neutralize_live_refresh(monkeypatch):
    # Only act if the Windows daemon is actually loaded in this session; the
    # macOS-only tests never import it and don't need (or want) it pulled in.
    mod = sys.modules.get(_WIN_MODULE)
    if mod is None:
        return
    # Proactive path: pretend "expiry unknown" so the near-expiry branch is skipped.
    monkeypatch.setattr(mod, "_credentials_expiry_seconds", lambda raw=None: None)
    # Reactive path: a no-op refresh that recovers nothing, so an AuthError still
    # falls through to the existing "token expired" toast (the prior contract).
    monkeypatch.setattr(mod, "refresh_access_token", AsyncMock(return_value=None))
