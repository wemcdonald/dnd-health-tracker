"""Tests for the supervisor's pure decisions (run under CPython).

    cd firmware-esp32 && python3 tests/test_main.py
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import main  # noqa: E402


def test_choose_mode():
    assert main.choose_mode(True, True) == "run"
    assert main.choose_mode(True, False) == "setup"
    assert main.choose_mode(False, True) == "setup"


def test_idle_setup_with_saved_wifi_resets_after_timeout():
    # Fresh board awaiting a slug, or a router outage: reboot to retry.
    assert main.setup_should_reset(True, main.SETUP_IDLE_RESET_S + 1)


def test_idle_setup_waits_out_the_timeout():
    assert not main.setup_should_reset(True, 0)
    assert not main.setup_should_reset(True, main.SETUP_IDLE_RESET_S)


def test_setup_without_saved_wifi_serves_forever():
    assert not main.setup_should_reset(False, 10 * main.SETUP_IDLE_RESET_S)


def test_idle_is_measured_from_setup_start_without_activity():
    assert main.setup_idle_s(200, None) == 200


def test_idle_is_measured_from_the_later_of_start_and_activity():
    # A phone that auto-opened the page 30 s ago only delays the retry.
    assert main.setup_idle_s(500, 30) == 30
    assert main.setup_idle_s(10, 30) == 10  # activity stamped before start: start wins


def test_activity_long_ago_no_longer_blocks_the_reset():
    idle = main.setup_idle_s(1000, main.SETUP_IDLE_RESET_S + 5)
    assert main.setup_should_reset(True, idle)


if __name__ == "__main__":
    fns = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    for fn in fns:
        fn()
        print("ok", fn.__name__)
    print("PASS", len(fns), "tests")
