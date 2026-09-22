"""Provider-agnostic usage collection.

Petmeter shows usage for more than one coding-agent plan (Claude Code today,
Codex next, Cursor later). Each provider exposes its numbers differently --
Anthropic returns them as response headers on any API call, Codex serves a
JSON usage endpoint -- so each gets a Collector, and the daemon only ever sees
the normalized UsageSnapshot below.

Free-ride credential rule (inherited from the Claude collector, see
daemon/tests/test_freeride.py): a collector NEVER refreshes an OAuth token.
The CLI that owns the token does all refreshing; a collector reads whatever
access token is currently stored and reports `None` when it is dead, which the
daemon renders as "No data" on the device. A collector that refreshes tokens
races the owning CLI and can invalidate its session.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Protocol

# Window labels every provider normalizes onto. Anthropic reports 5h/7d
# directly; Codex reports window lengths in seconds that map onto the same two.
WINDOW_5H = "5h"
WINDOW_7D = "7d"


@dataclass(frozen=True)
class Window:
    """One rate-limit window: how much is used and when it resets."""

    used_percent: float
    # Seconds until reset, relative -- immune to clock skew between this host
    # and the provider. None when the provider gives no reset time.
    resets_in: int | None = None

    @property
    def resets_at(self) -> float | None:
        """Absolute reset time, for display only."""
        import time

        return None if self.resets_in is None else time.time() + self.resets_in


@dataclass(frozen=True)
class UsageSnapshot:
    """One provider's usage at one moment, normalized for the device payload."""

    provider: str                       # "claude" | "codex" | "cursor"
    plan: str | None = None             # e.g. "pro", "max"
    windows: dict[str, Window] = field(default_factory=dict)

    # Where the number came from, and whether it was read on demand.
    # Per-model quotas, keyed by model name. Some plans meter a specific model
    # separately from the account: a Codex Pro account has one account-wide
    # weekly limit plus a GPT-5.3-Codex-Spark bucket with its own 5h and weekly
    # windows. Kept apart from `windows` because they are different quotas --
    # merging them would report one limit's headroom against another's usage.
    model_windows: dict[str, dict[str, Window]] = field(default_factory=dict)

    # Grants that restore a spent quota, where the provider offers them.
    # Codex hands these out as "full resets"; Claude has no equivalent, so the
    # fields stay at their defaults and the device shows nothing.
    reset_credits: int = 0
    reset_credits_expire: float | None = None   # soonest expiry, epoch seconds

    source: str = "unknown"

    # Set by the collector, never inferred from stale_seconds: a log file
    # written a moment ago is zero seconds old but still is not a live read,
    # and the device must not present it as one.
    live: bool = True

    # Age of the underlying observation, for "as of 3h ago" rendering. Always
    # 0 when live.
    stale_seconds: int = 0


class Collector(Protocol):
    """What the daemon requires of every provider.

    `collect` is async because the daemon's loop is asyncio-driven (bleak owns
    the BLE connection). Blocking work -- HTTP, and reading rollout logs that
    can run to hundreds of megabytes -- must be offloaded rather than run on
    the event loop, or it stalls the device link while it runs.
    """

    provider: str

    async def collect(self) -> UsageSnapshot | None:
        """Current usage, or None if unavailable (signed out, token dead,
        provider unreachable). Must not raise for expected failures, and must
        never refresh a token it does not own."""
        ...
