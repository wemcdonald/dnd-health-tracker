"""Tests for Cobalt-token auth (run under CPython).

    cd firmware && python3 tests/test_auth.py
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import auth  # noqa: E402
import ddb  # noqa: E402


class FakeSock:
    def __init__(self, data):
        self.data = data
        self.pos = 0

    def write(self, b):
        pass

    def read(self, n):
        end = min(self.pos + n, len(self.data))
        out = self.data[self.pos:end]
        self.pos = end
        return out

    def close(self):
        pass


def _resp(json_body, status=200):
    body = json_body.encode()
    return (b"HTTP/1.1 %d OK\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n"
            % (status, len(body))) + body


def test_token_fetch_cache_refresh():
    clock = [1000.0]
    calls = {"n": 0}
    responses = [_resp('{"token":"tok-A","ttl":600}'),
                 _resp('{"token":"tok-B","ttl":600}')]

    def connect(host, port):
        assert host == auth.AUTH_HOST
        r = responses[min(calls["n"], len(responses) - 1)]
        calls["n"] += 1
        return FakeSock(r)

    a = auth.CookieAuth("CobaltSession=xyz", connect=connect, now=lambda: clock[0])
    assert a.get_token() == "tok-A"
    assert calls["n"] == 1
    # cached: no new request, expiry = now + ttl - safety
    assert a.get_token() == "tok-A" and calls["n"] == 1
    assert a.expires == 1000.0 + 600 - auth.TOKEN_SAFETY
    # advance past expiry -> refresh fetches a new token
    clock[0] = a.expires + 1
    assert a.get_token() == "tok-B" and calls["n"] == 2


def test_missing_cookie_raises():
    a = auth.CookieAuth("", connect=lambda h, p: FakeSock(b""))
    try:
        a.get_token()
        raise AssertionError("expected ValueError")
    except ValueError:
        pass


def test_http_error_raises():
    def connect(host, port):
        return FakeSock(_resp("{}", status=403))
    a = auth.CookieAuth("c", connect=connect)
    try:
        a.get_token()
        raise AssertionError("expected HTTPStatusError")
    except ddb.HTTPStatusError as e:
        assert e.code == 403


def main():
    test_token_fetch_cache_refresh()
    test_missing_cookie_raises()
    test_http_error_raises()
    print("test_auth: OK")


if __name__ == "__main__":
    main()
