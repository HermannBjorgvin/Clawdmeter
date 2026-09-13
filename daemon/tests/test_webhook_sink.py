#!/usr/bin/env python3
"""Unit tests for the optional webhook sink (`webhook_url` config option).

Covers read_webhook_url, post_webhook, and the display-less webhook_only_cycle.

Run: python -m pytest daemon/tests/test_webhook_sink.py -x -q
"""
import asyncio
from unittest.mock import AsyncMock, MagicMock, patch

import httpx

import daemon.claude_usage_daemon as mod
from daemon.claude_usage_daemon import post_webhook, read_webhook_url, webhook_only_cycle


def _run(coro):
    return asyncio.run(coro)


# --- read_webhook_url -------------------------------------------------------

def test_webhook_url_none_when_config_absent(tmp_path, monkeypatch):
    monkeypatch.setattr(mod, "CONFIG_FILE", tmp_path / "config")
    assert read_webhook_url() is None


def test_webhook_url_none_when_key_absent(tmp_path, monkeypatch):
    cfg = tmp_path / "config"
    cfg.write_text("chime = on\n")
    monkeypatch.setattr(mod, "CONFIG_FILE", cfg)
    assert read_webhook_url() is None


def test_webhook_url_parsed_with_comment_and_whitespace(tmp_path, monkeypatch):
    cfg = tmp_path / "config"
    cfg.write_text("  WEBHOOK_URL =  https://ha.local:8123/api/webhook/abc  # HA\n")
    monkeypatch.setattr(mod, "CONFIG_FILE", cfg)
    assert read_webhook_url() == "https://ha.local:8123/api/webhook/abc"


def test_webhook_url_rejects_non_http(tmp_path, monkeypatch):
    cfg = tmp_path / "config"
    cfg.write_text("webhook_url = ftp://nope\n")
    monkeypatch.setattr(mod, "CONFIG_FILE", cfg)
    assert read_webhook_url() is None


# --- post_webhook -----------------------------------------------------------

def _client_returning(resp=None, exc=None):
    client = MagicMock()
    client.__aenter__ = AsyncMock(return_value=client)
    client.__aexit__ = AsyncMock(return_value=False)
    if exc is not None:
        client.post = AsyncMock(side_effect=exc)
    else:
        client.post = AsyncMock(return_value=resp)
    return client


def test_post_webhook_ok_sends_json_body():
    resp = MagicMock(status_code=200, text="")
    client = _client_returning(resp)
    with patch.object(mod.httpx, "AsyncClient", return_value=client):
        assert _run(post_webhook("https://x/hook", {"s": 3, "w": 11})) is True
    client.post.assert_awaited_once_with("https://x/hook", json={"s": 3, "w": 11})


def test_post_webhook_http_error_is_swallowed():
    resp = MagicMock(status_code=404, text="not found")
    with patch.object(mod.httpx, "AsyncClient", return_value=_client_returning(resp)):
        assert _run(post_webhook("https://x/hook", {"s": 1})) is False


def test_post_webhook_transport_error_is_swallowed():
    client = _client_returning(exc=httpx.ConnectError("refused"))
    with patch.object(mod.httpx, "AsyncClient", return_value=client):
        assert _run(post_webhook("https://x/hook", {"s": 1})) is False


# --- webhook_only_cycle -----------------------------------------------------

def test_webhook_only_cycle_posts_polled_payload():
    with patch.object(mod, "poll_active_payload", AsyncMock(return_value={"s": 5, "w": 9})), \
         patch.object(mod, "post_webhook", AsyncMock(return_value=True)) as post:
        assert _run(webhook_only_cycle("https://x/hook")) is True
    post.assert_awaited_once_with("https://x/hook", {"s": 5, "w": 9})


def test_webhook_only_cycle_skips_when_no_payload():
    with patch.object(mod, "poll_active_payload", AsyncMock(return_value=None)), \
         patch.object(mod, "post_webhook", AsyncMock()) as post:
        assert _run(webhook_only_cycle("https://x/hook")) is False
    post.assert_not_awaited()
