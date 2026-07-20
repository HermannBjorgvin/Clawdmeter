from __future__ import annotations

import math
import time
from datetime import datetime, timezone
from numbers import Real

import httpx


FABLE_USAGE_URL = "https://api.anthropic.com/api/oauth/usage"
FABLE_CACHE_SECONDS = 180.0

_cached_usage: dict[str, int] | None = None
_cached_at = 0.0


def _reset_cache() -> None:
    global _cached_usage, _cached_at
    _cached_usage = None
    _cached_at = 0.0


def extract_fable_usage(
    value: object,
    now: datetime | None = None,
) -> dict[str, int]:
    if not isinstance(value, dict) or not isinstance(value.get("limits"), list):
        return {}

    current = now or datetime.now(timezone.utc)
    for limit in value["limits"]:
        if not isinstance(limit, dict):
            continue
        scope = limit.get("scope")
        model = scope.get("model") if isinstance(scope, dict) else None
        percent = limit.get("percent")
        if (
            limit.get("kind") != "weekly_scoped"
            or not isinstance(model, dict)
            or model.get("display_name") != "Fable"
            or isinstance(percent, bool)
            or not isinstance(percent, Real)
        ):
            continue

        result = {"f": max(0, min(100, int(round(float(percent)))))}
        resets_at = limit.get("resets_at")
        if isinstance(resets_at, str):
            try:
                reset = datetime.fromisoformat(resets_at.replace("Z", "+00:00"))
                if reset.tzinfo is None:
                    reset = reset.replace(tzinfo=timezone.utc)
                result["fr"] = max(
                    0,
                    math.ceil(
                        (reset.astimezone(timezone.utc) - current).total_seconds()
                        / 60
                    ),
                )
            except ValueError:
                pass
        return result
    return {}


async def poll_fable_usage(token: str) -> dict[str, int]:
    global _cached_usage, _cached_at

    monotonic_now = time.monotonic()
    if (
        _cached_usage is not None
        and monotonic_now - _cached_at < FABLE_CACHE_SECONDS
    ):
        return dict(_cached_usage)

    headers = {
        "Authorization": f"Bearer {token}",
        "anthropic-beta": "oauth-2025-04-20",
    }
    try:
        async with httpx.AsyncClient(timeout=5.0) as client:
            response = await client.get(FABLE_USAGE_URL, headers=headers)
        if response.status_code != 200:
            return {}
        parsed = extract_fable_usage(response.json())
    except (httpx.HTTPError, ValueError):
        return {}
    if not parsed:
        return {}

    _cached_usage = parsed
    _cached_at = monotonic_now
    return dict(parsed)
