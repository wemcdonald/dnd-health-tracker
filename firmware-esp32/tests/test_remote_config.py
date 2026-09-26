"""Tests for server-pushed config (run under CPython).

    cd firmware-esp32 && python3 tests/test_remote_config.py
"""

import json
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import config  # noqa: E402
import device  # noqa: E402
import remote_config  # noqa: E402


def _dev(**kw):
    d = dict(config.DEFAULT_DEVICE)
    d.update(kw)
    return config.Device(d)


def _net(ssid, psk="pw", priority=0):
    return {"ssid": ssid, "psk": psk, "priority": priority}


def test_applies_slug_brightness_poll():
    r = remote_config.apply({"rev": 3, "slug": "shen", "brightness": 0.4, "poll_seconds": 10},
                            _dev(slug="nan"), [])
    assert (r.dev.slug, r.dev.brightness, r.dev.poll_seconds) == ("shen", 0.4, 10.0)
    assert (r.rev, r.dev_changed, r.wifi_changed) == (3, True, False)


def test_absent_or_null_fields_keep_local():
    r = remote_config.apply({"rev": 1, "slug": None}, _dev(slug="nan", brightness=0.5), [])
    assert r.dev.slug == "nan" and r.dev.brightness == 0.5
    assert not r.dev_changed


def test_invalid_slug_skipped_but_rev_accepted():
    r = remote_config.apply({"rev": 9, "slug": "Bad Slug!"}, _dev(slug="nan"), [])
    assert r.dev.slug == "nan" and r.rev == 9 and not r.dev_changed


def test_clamps_and_rejects_non_numbers():
    r = remote_config.apply({"rev": 1, "brightness": 7, "poll_seconds": 0.1}, _dev(), [])
    assert r.dev.brightness == 1.0 and r.dev.poll_seconds == 2.0
    r = remote_config.apply({"rev": 1, "brightness": True, "poll_seconds": "5"}, _dev(), [])
    assert r.dev.brightness == 0.5 and r.dev.poll_seconds == 5.0 and not r.dev_changed


def test_wifi_upsert_adds_and_updates():
    r = remote_config.apply(
        {"rev": 1, "wifi": {"upsert": [_net("Home", "new", 1), _net("Cabin", "pw", 2)]}},
        _dev(), [_net("Home", "old", 1)])
    assert r.nets == [_net("Cabin", "pw", 2), _net("Home", "new", 1)]
    assert r.wifi_changed


def test_wifi_upsert_never_drops_local_networks():
    r = remote_config.apply({"rev": 1, "wifi": {"upsert": [_net("Cabin")]}},
                            _dev(), [_net("Local")])
    assert sorted(n["ssid"] for n in r.nets) == ["Cabin", "Local"]


def test_wifi_remove_skips_connected_network():
    r = remote_config.apply({"rev": 1, "wifi": {"remove": ["A", "B"]}},
                            _dev(), [_net("A"), _net("B")], connected_ssid="A")
    assert [n["ssid"] for n in r.nets] == ["A"]
    assert r.wifi_changed


def test_identical_wifi_is_not_a_change():
    r = remote_config.apply({"rev": 2, "wifi": {"upsert": [_net("Home")]}}, _dev(), [_net("Home")])
    assert not r.wifi_changed


def test_malformed_wifi_entries_ignored():
    r = remote_config.apply({"rev": 1, "wifi": {"upsert": [{"psk": "x"}, "junk", {"ssid": ""}],
                                                "remove": [5]}},
                            _dev(), [_net("Home")])
    assert r.nets == [_net("Home")] and not r.wifi_changed
    r = remote_config.apply({"rev": 1, "wifi": "junk"}, _dev(), [_net("Home")])
    assert r.nets == [_net("Home")]


def test_wifi_upsert_remove_non_list_ignored_without_crashing():
    # Regression: a naive `wifi.get("upsert") or []` treats a truthy non-list
    # (e.g. an int) as iterable and raises TypeError. Must be ignored instead.
    r = remote_config.apply({"rev": 1, "wifi": {"upsert": 5, "remove": 5}},
                            _dev(), [_net("Home")])
    assert r.nets == [_net("Home")] and not r.wifi_changed


def test_parse_body():
    assert remote_config.parse_body('{"rev": 1}') == {"rev": 1}
    assert remote_config.parse_body("[1]") is None
    assert remote_config.parse_body("not json") is None
    assert remote_config.parse_body('{"rev": "x"}') is None


def _check(data_dir, responder, dev=None):
    ident = device.load_identity(data_dir, token_factory=lambda: "a" * 32)
    calls = []

    def fake_http(host, path, port=80, timeout=8, headers=None):
        calls.append((host, path, headers))
        return responder()

    new = remote_config.check(dev or _dev(slug="nan"), ident, mac="ac276e7d1774", fw_version=2,
                              connected_ssid="Home", local_ip="10.0.10.93",
                              data_dir=data_dir, http=fake_http)
    return new, calls


def test_check_applies_persists_and_sends_headers():
    with tempfile.TemporaryDirectory() as d:
        body = json.dumps({"rev": 4, "slug": "shen",
                           "wifi": {"upsert": [_net("Cabin")]}})
        new, calls = _check(d, lambda: (200, {}, body))
        assert new is not None and new.slug == "shen"
        assert config.load_device(d).slug == "shen"
        assert [n["ssid"] for n in config.load_wifi(d)] == ["Cabin"]
        assert device.load_identity(d)["config_rev"] == 4
        host, path, headers = calls[0]
        assert (host, path) == ("dndhealth.willflix.org", "/device/ac276e7d1774/config")
        assert headers == {"X-Device-Token": "a" * 32, "X-Config-Rev": "0",
                           "X-Firmware-Version": "2", "X-Slug": "nan",
                           "X-Local-IP": "10.0.10.93"}


def test_check_wifi_only_change_returns_none_but_saves():
    with tempfile.TemporaryDirectory() as d:
        body = json.dumps({"rev": 2, "wifi": {"upsert": [_net("Cabin")]}})
        new, _ = _check(d, lambda: (200, {}, body))
        assert new is None
        assert [n["ssid"] for n in config.load_wifi(d)] == ["Cabin"]
        assert device.load_identity(d)["config_rev"] == 2


def test_check_304_403_errors_change_nothing():
    with tempfile.TemporaryDirectory() as d:
        for responder in (lambda: (304, {}, ""), lambda: (403, {}, "token mismatch\n"),
                          lambda: None, lambda: (200, {}, "not json")):
            new, _ = _check(d, responder)
            assert new is None
        assert device.load_identity(d)["config_rev"] == 0


def test_check_never_raises():
    def boom():
        raise OSError("network down")
    with tempfile.TemporaryDirectory() as d:
        new, _ = _check(d, boom)
        assert new is None


if __name__ == "__main__":
    fns = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    for fn in fns:
        fn()
        print("ok", fn.__name__)
    print("PASS", len(fns), "tests")
