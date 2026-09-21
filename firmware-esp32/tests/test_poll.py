"""Tests for the device-feed wire parser (run under CPython).

    cd firmware-esp32 && python3 tests/test_poll.py
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import poll  # noqa: E402
import config  # noqa: E402


def test_parses_four_ints():
    assert poll.parse_feed("25 40 3 7\nHP 25/40 (+3 temp)") == (25, 40, 3, 7)


def test_ignores_lines_after_first():
    assert poll.parse_feed("16 32 0 2\nanything\nhere") == (16, 32, 0, 2)


def test_bytes_body():
    assert poll.parse_feed(b"1 10 0 0\n") == (1, 10, 0, 0)


def test_down_is_valid():
    # 0 current HP is a live character at 0 -> valid (bar shows "down").
    assert poll.parse_feed("0 40 0 4\nDOWN") == (0, 40, 0, 4)


def test_sentinel_is_offline():
    # "0 0 0 99999" (max==0) means the server has no upstream data yet.
    assert poll.parse_feed("0 0 0 99999\nHP unknown") is None


def test_rejects_short_line():
    assert poll.parse_feed("16 32 0") is None


def test_rejects_garbage():
    assert poll.parse_feed("not a number\n") is None
    assert poll.parse_feed("") is None
    assert poll.parse_feed(None) is None


def test_rejects_negative():
    assert poll.parse_feed("-1 40 0 0") is None
    assert poll.parse_feed("5 40 -1 0") is None


def test_device_url():
    dev = config.Device({"slug": "alishba", "server_host": "dndhealth.willflix.org"})
    assert dev.url == "http://dndhealth.willflix.org/alishba.txt"
    dev2 = config.Device({"slug": "shen", "server_host": "10.0.0.5", "server_port": 8080})
    assert dev2.url == "http://10.0.0.5:8080/shen.txt"


if __name__ == "__main__":
    fns = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    for fn in fns:
        fn()
        print("ok", fn.__name__)
    print("PASS", len(fns), "tests")
