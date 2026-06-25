"""Tests for the DDB HTTP layer + adaptive poller (run under CPython).

    cd firmware && python3 tests/test_ddb.py

No network: a fake socket replays canned HTTP responses, and the poller runs
against a fake fetch/clock/sleep so cadence and backoff are deterministic.
"""

import asyncio
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import ddb  # noqa: E402

FIXTURE = os.path.join(os.path.dirname(__file__), "fixture_character.json")


class FakeSock:
    """Replays `data` in small slices (to exercise the buffering/refill paths)."""

    def __init__(self, data, slice_cap=40):
        self.data = data
        self.pos = 0
        self.cap = slice_cap

    def write(self, b):
        pass

    def read(self, n):
        if self.pos >= len(self.data):
            return b""
        end = min(self.pos + min(n, self.cap), len(self.data))
        out = self.data[self.pos:end]
        self.pos = end
        return out

    def close(self):
        pass


def _chunked(body, size=64):
    parts = []
    for i in range(0, len(body), size):
        c = body[i:i + size]
        parts.append(b"%x\r\n%s\r\n" % (len(c), c))
    parts.append(b"0\r\n\r\n")
    head = b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nTransfer-Encoding: chunked\r\n\r\n"
    return head + b"".join(parts)


def _content_length(body):
    head = b"HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n" % len(body)
    return head + body


def test_fetch_chunked_and_plain():
    with open(FIXTURE, "rb") as f:
        body = f.read()

    for resp in (_chunked(body, 64), _chunked(body, 7), _content_length(body)):
        def connect(host, port, _r=resp):
            return FakeSock(_r)
        assert ddb.fetch_hp("12345678", connect=connect) == (50, 63, 9)


def test_fetch_private_raises():
    def connect(host, port):
        return FakeSock(b"HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n")
    try:
        ddb.fetch_hp("12345678", connect=connect)
        raise AssertionError("expected AuthError")
    except ddb.AuthError:
        pass


def _make_poller(fetch, intervals):
    async def sleep(d):
        intervals.append(round(d, 6))

    hps, statuses = [], []
    p = ddb.Poller(
        fetch, "id",
        on_hp=hps.append, on_status=statuses.append,
        interval=5.0, err_backoff=2.0,
        rng=lambda: 0.5,  # -> zero jitter, deterministic
        sleep=sleep,
    )
    return p, hps, statuses


def test_cadence_steady():
    intervals = []
    p, hps, statuses = _make_poller(lambda cid: (10, 20, 0), intervals)

    def stop():
        return len(intervals) >= 5
    asyncio.run(p.run(should_stop=stop))
    # steady 5s, no idle backoff; a constant value is reported exactly once
    assert hps == [(10, 20, 0)], hps
    assert intervals[:5] == [5.0] * 5, intervals


def test_detects_each_change():
    seq = [(10, 20, 0), (10, 20, 0), (8, 20, 0), (8, 20, 0), (12, 20, 3)]
    box = {"i": 0}

    def fetch(cid):
        v = seq[min(box["i"], len(seq) - 1)]
        box["i"] += 1
        return v
    intervals = []
    p, hps, statuses = _make_poller(fetch, intervals)

    def stop():
        return len(intervals) >= 5
    asyncio.run(p.run(should_stop=stop))
    assert hps == [(10, 20, 0), (8, 20, 0), (12, 20, 3)], hps


def test_error_backoff_doubles():
    intervals = []

    def boom(cid):
        raise OSError("network down")
    p, hps, statuses = _make_poller(boom, intervals)

    def stop():
        return len(intervals) >= 6
    asyncio.run(p.run(should_stop=stop))
    assert intervals == [2.0, 4.0, 8.0, 16.0, 32.0, 60.0], intervals
    assert hps == [] and statuses[:1] == [False]


def test_nudge_interrupts_wait():
    # With an Event supplied, nudge() must cut a long wait short (the websocket
    # path uses this to force an immediate fetch on a live event).
    ev = asyncio.Event()
    p = ddb.Poller(lambda cid: (1, 1, 0), "id",
                   on_hp=lambda s: None, on_status=lambda o: None,
                   interval=100.0, rng=lambda: 0.5, event=ev)

    async def scenario():
        task = asyncio.ensure_future(p._wait(100.0))
        await asyncio.sleep(0.01)
        p.nudge()
        await asyncio.wait_for(task, 1.0)  # returns promptly, not after 100s
    asyncio.run(scenario())


def main():
    test_fetch_chunked_and_plain()
    test_fetch_private_raises()
    test_cadence_steady()
    test_detects_each_change()
    test_error_backoff_doubles()
    test_nudge_interrupts_wait()
    print("test_ddb: OK")


if __name__ == "__main__":
    main()
