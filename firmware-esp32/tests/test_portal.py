"""Tests for the setup portal's pure parts (run under CPython).

    cd firmware-esp32 && python3 tests/test_portal.py
"""

import asyncio
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import config  # noqa: E402
import portal  # noqa: E402


def _seed(d, **kw):
    dev = dict(config.DEFAULT_DEVICE)
    dev.update(kw)
    config.save_device(config.Device(dev), d)


def test_parse_form_unquotes():
    assert portal.parse_form(b"ssid=My+Net&psk=a%26b") == {"ssid": "My Net", "psk": "a&b"}


def test_save_slug_is_stripped_and_lowered():
    with tempfile.TemporaryDirectory() as d:
        assert portal.apply_save({"slug": "  Shen "}, d) == "device"
        assert config.load_device(d).slug == "shen"


def test_save_rejects_slug_with_crlf_and_keeps_old():
    with tempfile.TemporaryDirectory() as d:
        _seed(d, slug="nan")
        assert portal.apply_save({"slug": "evil\r\nX-Other: 1"}, d) == portal.BAD_SLUG
        assert config.load_device(d).slug == "nan"


def test_save_rejects_bad_chars_and_overlong_slug():
    with tempfile.TemporaryDirectory() as d:
        _seed(d, slug="nan")
        assert portal.apply_save({"slug": "has space"}, d) == portal.BAD_SLUG
        assert portal.apply_save({"slug": "a" * 65}, d) == portal.BAD_SLUG
        assert config.load_device(d).slug == "nan"
        assert portal.apply_save({"slug": "a" * 64}, d) == "device"


def test_bad_slug_saves_nothing_else():
    # The whole form is rejected so the message is shown instead of a reboot.
    with tempfile.TemporaryDirectory() as d:
        assert portal.apply_save({"ssid": "Home", "psk": "pw", "slug": "bad slug"}, d) \
            == portal.BAD_SLUG
        assert config.load_wifi(d) == []


def test_blank_slug_keeps_existing():
    with tempfile.TemporaryDirectory() as d:
        _seed(d, slug="nan")
        assert portal.apply_save({"ssid": "Home", "psk": "pw", "slug": " "}, d) == "wifi"
        assert config.load_device(d).slug == "nan"


def test_bad_slug_response_shows_message_and_does_not_reboot():
    with tempfile.TemporaryDirectory() as d:
        p = portal.Portal(None, d)
        resp = p._respond("POST", "/", b"slug=bad+slug")
        assert b"200 OK" in resp and b"Nothing was saved" in resp
        assert not p._should_reboot


def test_activity_rule():
    assert portal.is_activity("POST", "/")
    assert portal.is_activity("POST", "/anything")
    assert portal.is_activity("GET", "/")
    assert not portal.is_activity("GET", "/generate_204")
    assert not portal.is_activity("GET", "/hotspot-detect.html")


class _Reader:
    def __init__(self, raw):
        self.lines = raw.split(b"\r\n")
        self.lines = [ln + b"\r\n" for ln in self.lines[:-1]] + [self.lines[-1]]

    async def readline(self):
        return self.lines.pop(0) if self.lines else b""

    async def readexactly(self, n):
        return self.lines.pop(0)[:n] if self.lines else b""


class _Writer:
    def __init__(self):
        self.out = []

    def write(self, data):
        self.out.append(data)

    async def drain(self):
        pass

    async def aclose(self):
        pass


def _serve_one(p, raw):
    w = _Writer()
    asyncio.run(p._http_client(_Reader(raw), w))
    return w.out


def test_probe_redirect_is_not_activity():
    with tempfile.TemporaryDirectory() as d:
        p = portal.Portal(None, d, ticks=lambda: 42)
        out = _serve_one(p, b"GET /generate_204 HTTP/1.1\r\nHost: x\r\n\r\n")
        assert "302" in out[0]  # redirects are str
        assert p.last_activity is None


def test_form_page_records_activity():
    with tempfile.TemporaryDirectory() as d:
        p = portal.Portal(None, d, ticks=lambda: 42)
        out = _serve_one(p, b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")
        assert b"200 OK" in out[0]
        assert p.last_activity == 42


def test_post_records_activity_before_it_is_handled():
    with tempfile.TemporaryDirectory() as d:
        p = portal.Portal(None, d, ticks=lambda: 7)
        seen = []
        real = p._respond

        def spy(method, path, body):
            seen.append(p.last_activity)  # already stamped when handling starts
            return real(method, path, body)

        p._respond = spy
        _serve_one(p, b"POST / HTTP/1.1\r\nContent-Length: 3\r\n\r\na=b")
        assert seen == [7] and p.last_activity == 7


if __name__ == "__main__":
    fns = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    for fn in fns:
        fn()
        print("ok", fn.__name__)
    print("PASS", len(fns), "tests")
