#!/usr/bin/env python3
"""Attention alerts consumed by the daemon. Run: python -m pytest daemon/tests/test_attention.py -q"""
import json
import os
import time
from unittest.mock import patch

from daemon import claude_usage_daemon as d


def test_attention_flag(tmp_path):
    flag = tmp_path / "attention"
    with patch.object(d, "ATTENTION_FLAG", flag):
        assert d.take_attention_flag() is None   # absent

        flag.touch()
        assert d.take_attention_flag()["m"] == ""   # bare touch still beeps
        assert not flag.exists()                    # consumed

        flag.write_text(json.dumps({"m": "Você  aprova\na ação?", "s": "abc"}))
        alert = d.take_attention_flag()
        assert (alert["m"], alert["s"]) == ("Voce aprova a acao?", "abc")
        assert d.to_ascii("x" * 200, 120) == "x" * 117 + "..."

        flag.touch()
        old = time.time() - d.ATTENTION_MAX_AGE - 1
        os.utime(flag, (old, old))
        assert d.take_attention_flag() is None   # stale
        assert not flag.exists()


def test_clears(tmp_path):
    with patch.object(d, "ATTENTION_DIR", tmp_path):
        (tmp_path / "clear-abc").touch()
        assert set(d.take_clears()) == {"abc"}
        assert d.take_clears() == {}             # consumed


def test_encode_payload_fits():
    base = {"s": 4, "ok": True, "b": 1}
    short = d.encode_payload({**base, "m": "hi"}, 180)
    assert json.loads(short)["m"] == "hi"

    long = d.encode_payload({**base, "m": "x" * 200}, 120)
    assert len(long) <= 120
    assert json.loads(long)["m"].endswith("...")

    tiny = d.encode_payload({**base, "m": "x" * 200}, 26)
    assert "m" not in json.loads(tiny)
