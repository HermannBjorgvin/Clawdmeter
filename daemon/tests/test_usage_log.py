#!/usr/bin/env python3
"""Unit tests for the opt-in usage log (`usage_log` config → usage_log.jsonl).

Covers the config gate (off by default, on when set) and _log_usage behavior:
nothing is written unless opted in, and only usage fields are recorded.

Run: python -m pytest daemon/tests/test_usage_log.py -x -q
"""
import json
from unittest.mock import patch

from daemon import claude_usage_daemon_windows as d


def _config(tmp_path, body):
    cfg = tmp_path / "config"
    cfg.write_text(body, encoding="utf-8")
    return cfg


def test_setting_defaults_off(tmp_path):
    with patch.object(d, "CONFIG_FILE", _config(tmp_path, "chime = on\n")):
        assert d.read_usage_log_setting() == "off"


def test_setting_reads_on(tmp_path):
    with patch.object(d, "CONFIG_FILE", _config(tmp_path, "usage_log = on\n")):
        assert d.read_usage_log_setting() == "on"


def test_log_skips_when_off(tmp_path):
    logf = tmp_path / "usage_log.jsonl"
    with patch.object(d, "CONFIG_FILE", _config(tmp_path, "usage_log = off\n")), \
         patch.object(d, "USAGE_LOG_FILE", logf):
        d._log_usage({"s": 42, "w": 10})
    assert not logf.exists()


def test_log_writes_when_on(tmp_path):
    logf = tmp_path / "usage_log.jsonl"
    with patch.object(d, "CONFIG_FILE", _config(tmp_path, "usage_log = on\n")), \
         patch.object(d, "USAGE_LOG_FILE", logf):
        d._log_usage({"s": 42, "w": 10, "acct": "pro", "ok": True})
        d._log_usage({"s": 55, "w": 12, "acct": "pro"})
    lines = logf.read_text(encoding="utf-8").strip().splitlines()
    assert len(lines) == 2
    rec = json.loads(lines[0])
    assert rec["s"] == 42 and rec["w"] == 10 and rec["acct"] == "pro"
    assert "ts" in rec and "iso" in rec
    assert "ok" not in rec  # only usage fields are logged, not transient flags
