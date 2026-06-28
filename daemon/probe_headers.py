#!/usr/bin/env python3
"""One-shot, read-only Anthropic rate-limit header probe.

Makes a SINGLE max_tokens:1 call to /v1/messages with the currently logged-in
Claude Code OAuth token (the same call the daemon makes) and prints every
response header plus the exact JSON payload the daemon WOULD send over BLE.

Read-only: no BLE, no device, ONE API call per run. Run it manually, one account
at a time, with the daemon stopped — so manual probes don't stack on the daemon's
60s polls and trip a 429/529. See DEBT.md D-1 / D-2 for why we capture these
samples (per-account header schema; usage-based Enterprise sends none).

Usage:
    python daemon/probe_headers.py [model]

`model` is optional. Default is the daemon's actual API_BODY model so the probe
faithfully reproduces what the daemon sends. Pass e.g. `claude-sonnet-4-6` to
use a current geo-routable model — useful on a data-residency / usage-based
Enterprise org where the daemon's older default 400s with "model not available
on the requested cloud provider" (DEBT.md D-2a), which would otherwise be
indistinguishable from the genuine no-headers case.
"""
import asyncio
import json
import sys
import time

import httpx

# Reuse the daemon's credential parsing + request constants (single source of
# truth). Importing the module pulls in bleak — run under the repo .venv.
from claude_usage_daemon_windows import (
    API_BODY,
    API_HEADERS_TEMPLATE,
    API_URL,
    read_token,
)


def derive_payload(headers, now):
    """Mirror of poll_api()'s header->payload mapping. Kept in sync by eye with
    claude_usage_daemon_windows.poll_api — if that mapping changes, update here."""
    H = "anthropic-ratelimit-unified-"

    def raw(suffix):
        return headers.get(H + suffix)

    def reset_minutes(reset_ts):
        try:
            r = float(reset_ts)
        except (TypeError, ValueError):
            return 0
        mins = (r - now) / 60.0
        return int(round(mins)) if mins > 0 else 0

    def pct(util):
        try:
            return int(round(float(util) * 100))
        except (TypeError, ValueError):
            return 0

    s_util = raw("5h-utilization")
    w_util = raw("7d-utilization")
    has_windows = s_util is not None or w_util is not None
    overage_in_use = raw("overage-in-use") == "true"
    status = raw("status") or raw("5h-status") or "unknown"
    if overage_in_use and s_util is None and w_util is None:
        s_util = w_util = "1.0"
        status = "overage"
    acct = "pro-max" if has_windows else ("ent" if overage_in_use else "pro-max")
    unified_reset = raw("reset")
    return {
        "s": pct(s_util),
        "sr": reset_minutes(raw("5h-reset") or unified_reset),
        "w": pct(w_util),
        "wr": reset_minutes(raw("7d-reset") or unified_reset),
        "o": pct(raw("overage-utilization")),
        "or": reset_minutes(unified_reset),
        "oiu": overage_in_use,
        "acct": acct,
        "st": status,
        "ok": True,
    }


def read_primary_api_key():
    """Read the Anthropic API key from ~/.claude.json `primaryApiKey` (Type 3 /
    Console-API login stores the key here, NOT as an OAuth accessToken in
    .credentials.json). Returns the key string or None. Never logged."""
    from pathlib import Path
    p = Path.home() / ".claude.json"
    try:
        data = json.loads(p.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None
    key = data.get("primaryApiKey") if isinstance(data, dict) else None
    return key if isinstance(key, str) and key.strip() else None


async def main():
    argv = sys.argv[1:]
    api_key_mode = "--api-key" in argv
    argv = [a for a in argv if a != "--api-key"]

    body = dict(API_BODY)
    if argv:
        body["model"] = argv[0]

    if api_key_mode:
        # Type 3: raw API key auth. x-api-key, NOT Bearer; drop the OAuth beta header.
        key = read_primary_api_key()
        if not key:
            print("NO API KEY — `primaryApiKey` not found in ~/.claude.json")
            return
        headers = dict(API_HEADERS_TEMPLATE)
        headers.pop("anthropic-beta", None)  # oauth beta header is OAuth-only
        headers["x-api-key"] = key
        auth_desc = f"x-api-key (sk-ant-…, len={len(key)})"  # never print the key itself
    else:
        token = read_token()
        if not token:
            print("NO TOKEN — is Claude Code logged in? (run `claude login`)")
            return
        headers = dict(API_HEADERS_TEMPLATE)
        headers["Authorization"] = f"Bearer {token}"
        auth_desc = "OAuth Bearer"

    print(f"Probing {API_URL} with model={body['model']}  auth={auth_desc} ...\n")

    try:
        async with httpx.AsyncClient(timeout=20.0) as http:
            resp = await http.post(API_URL, headers=headers, json=body)
    except httpx.HTTPError as e:
        print(f"REQUEST FAILED (transient/network): {e}")
        return

    print(f"HTTP {resp.status_code}\n")

    print("--- ALL response headers ---")
    for k in sorted(resp.headers):
        print(f"  {k}: {resp.headers[k]}")

    rl = {
        k: v
        for k, v in resp.headers.items()
        if "ratelimit" in k.lower() or k.lower() == "retry-after"
    }
    print("\n--- rate-limit / retry headers (the ones the device cares about) ---")
    if rl:
        for k in sorted(rl):
            print(f"  {k}: {rl[k]}")
    else:
        print("  (NONE — no anthropic-ratelimit-* headers present)")

    if resp.status_code < 400:
        payload = derive_payload(resp.headers, time.time())
        print("\n--- payload the daemon WOULD send over BLE ---")
        print("  " + json.dumps(payload, separators=(",", ":")))
    else:
        print(f"\nBody: {resp.text[:400]}")


if __name__ == "__main__":
    asyncio.run(main())
