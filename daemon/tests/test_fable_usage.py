from __future__ import annotations

import asyncio
from datetime import datetime, timezone
from unittest.mock import AsyncMock, MagicMock

import httpx

from daemon import fable_usage


NOW = datetime(2026, 7, 19, 3, 0, tzinfo=timezone.utc)


def fable_limit(percent: int = 61) -> dict:
    return {
        "kind": "weekly_scoped",
        "group": "weekly",
        "percent": percent,
        "severity": "normal",
        "resets_at": "2026-07-19T05:00:00+00:00",
        "scope": {"model": {"id": None, "display_name": "Fable"}},
        "is_active": True,
    }


def test_extract_fable_usage_reads_percent_and_reset_minutes() -> None:
    assert fable_usage.extract_fable_usage(
        {"limits": [fable_limit()]}, now=NOW
    ) == {"f": 61, "fr": 120}


def test_extract_fable_usage_preserves_measured_zero() -> None:
    assert fable_usage.extract_fable_usage(
        {"limits": [fable_limit(0)]}, now=NOW
    ) == {"f": 0, "fr": 120}


def test_extract_fable_usage_rejects_inactive_wrong_or_malformed_limits() -> None:
    inactive = fable_limit()
    inactive["is_active"] = False
    wrong = fable_limit()
    wrong["scope"]["model"]["display_name"] = "Opus"
    malformed = fable_limit()
    malformed["percent"] = "61"

    assert fable_usage.extract_fable_usage(
        {"limits": [inactive, wrong, malformed]}, now=NOW
    ) == {}


def test_extract_fable_usage_keeps_percent_when_reset_is_missing() -> None:
    limit = fable_limit()
    del limit["resets_at"]

    assert fable_usage.extract_fable_usage({"limits": [limit]}, now=NOW) == {
        "f": 61
    }


def test_poll_fable_usage_caches_success_for_180_seconds(monkeypatch) -> None:
    response = MagicMock()
    response.status_code = 200
    response.json.return_value = {"limits": [fable_limit()]}
    client = AsyncMock()
    client.__aenter__.return_value = client
    client.__aexit__.return_value = False
    client.get.return_value = response

    fable_usage._reset_cache()
    monkeypatch.setattr(fable_usage.httpx, "AsyncClient", lambda **_kwargs: client)
    monkeypatch.setattr(fable_usage.time, "monotonic", lambda: 100.0)

    first = asyncio.run(fable_usage.poll_fable_usage("token"))
    second = asyncio.run(fable_usage.poll_fable_usage("token"))

    assert first["f"] == 61
    assert second == first
    client.get.assert_awaited_once()


def test_poll_fable_usage_failure_returns_unavailable(monkeypatch) -> None:
    client = AsyncMock()
    client.__aenter__.return_value = client
    client.__aexit__.return_value = False
    client.get.side_effect = httpx.ConnectError("offline")

    fable_usage._reset_cache()
    monkeypatch.setattr(fable_usage.httpx, "AsyncClient", lambda **_kwargs: client)

    assert asyncio.run(fable_usage.poll_fable_usage("token")) == {}
