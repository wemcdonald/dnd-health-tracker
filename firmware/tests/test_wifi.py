"""Tests for WiFi station selection logic (run under CPython).

    cd firmware && python3 tests/test_wifi.py

Uses a fake STA so we can verify priority order and per-network timeout without
a radio.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import config  # noqa: E402
import wifi  # noqa: E402


class FakeSTA:
    """Connects only to `winner`, and only after `polls` isconnected() checks."""

    def __init__(self, winner=None, polls=2):
        self.winner = winner
        self.polls = polls
        self.attempts = []      # ssids passed to connect()
        self.disconnects = 0
        self._cur = None
        self._count = 0

    def active(self, *a):
        return True

    def connect(self, ssid, psk):
        self.attempts.append(ssid)
        self._cur = ssid
        self._count = 0

    def isconnected(self):
        if self._cur == self.winner:
            self._count += 1
            return self._count >= self.polls
        return False

    def disconnect(self):
        self.disconnects += 1
        self._cur = None


def _clock():
    t = [0.0]
    return (lambda: t[0]), (lambda d: t.__setitem__(0, t[0] + d))


def test_priority_order_and_fallback():
    nets = config.upsert_wifi([], "Cafe", "p1", 1)
    nets = config.upsert_wifi(nets, "Home", "p2", 5)
    # sorted: Home (prio 5) first, then Cafe (prio 1)
    assert [n["ssid"] for n in nets] == ["Home", "Cafe"]

    sta = FakeSTA(winner="Cafe", polls=2)
    now, sleep = _clock()
    got = wifi.connect_known(nets, timeout=5, settle=0.3, sta=sta, sleep=sleep, now=now)
    assert got == "Cafe", got
    assert sta.attempts == ["Home", "Cafe"], sta.attempts   # tried Home first
    assert sta.disconnects == 1                              # gave up on Home


def test_all_fail_returns_none():
    nets = config.upsert_wifi([], "Home", "p", 1)
    sta = FakeSTA(winner=None)
    now, sleep = _clock()
    got = wifi.connect_known(nets, timeout=2, settle=0.3, sta=sta, sleep=sleep, now=now)
    assert got is None
    assert sta.disconnects == 1


def test_empty_list():
    assert wifi.connect_known([], sta=FakeSTA()) is None


def main():
    test_priority_order_and_fallback()
    test_all_fail_returns_none()
    test_empty_list()
    print("test_wifi: OK")


if __name__ == "__main__":
    main()
