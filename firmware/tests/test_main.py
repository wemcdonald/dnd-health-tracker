"""Tests for the supervisor (run under CPython).

    cd firmware && python3 tests/test_main.py

Covers the pure mode-selection logic and smoke-tests the core-1 render loop
(threading stands in for _thread off-device).
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import anim  # noqa: E402
import config  # noqa: E402
import main  # noqa: E402 (import alone proves the module loads under CPython)

THEME = config.load_theme(os.path.join(os.path.dirname(__file__), "..", "data"))


class CaptureStrip:
    def __init__(self, n):
        self.n = n
        self.calls = 0

    def render(self, frame):
        assert len(frame) == self.n
        self.calls += 1

    def __len__(self):
        return self.n


def test_choose_mode():
    assert main.choose_mode(True, True) == "run"
    assert main.choose_mode(True, False) == "setup"
    assert main.choose_mode(False, True) == "setup"
    assert main.choose_mode(False, False) == "setup"


def test_render_loop_runs_and_stops():
    engine = anim.Engine(THEME, 16)
    strip = CaptureStrip(16)
    stop = [False]
    main._start_thread(main._render_loop, (engine, strip, 60, None, stop))
    time.sleep(0.25)
    stop[0] = True
    time.sleep(0.1)
    assert strip.calls > 0, "render loop never rendered"


def main_():
    test_choose_mode()
    test_render_loop_runs_and_stops()
    print("test_main: OK")


if __name__ == "__main__":
    main_()
