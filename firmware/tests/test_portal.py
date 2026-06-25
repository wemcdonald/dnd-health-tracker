"""Tests for the captive portal pure helpers (run under CPython).

    cd firmware && python3 tests/test_portal.py
"""

import os
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import config  # noqa: E402
import portal  # noqa: E402


def _dns_query(name):
    q = b"\xab\xcd" + b"\x01\x00" + b"\x00\x01" + b"\x00\x00" + b"\x00\x00" + b"\x00\x00"
    for label in name.split("."):
        q += bytes([len(label)]) + label.encode()
    q += b"\x00" + b"\x00\x01" + b"\x00\x01"  # root, QTYPE A, QCLASS IN
    return q


def test_dns_response():
    q = _dns_query("captive.apple.com")
    resp = portal.build_dns_response(q, portal.ip_to_bytes("192.168.4.1"))
    assert resp[0:2] == b"\xab\xcd"            # transaction id echoed
    assert resp[2:4] == b"\x81\x80"            # standard response, no error
    qd, an = struct.unpack(">H", resp[4:6])[0], struct.unpack(">H", resp[6:8])[0]
    assert qd == 1 and an == 1
    assert resp[-4:] == bytes([192, 168, 4, 1])  # A record points at the AP
    assert portal.build_dns_response(b"short", b"\x01\x02\x03\x04") == b""


def test_parse_form():
    f = portal.parse_form("ssid=My+Net&psk=p%40ss%21&character_id=12345678&empty=")
    assert f["ssid"] == "My Net"
    assert f["psk"] == "p@ss!"
    assert f["character_id"] == "12345678"
    assert f["empty"] == ""
    assert portal.parse_form("") == {}


def test_apply_save_roundtrip():
    d = tempfile.mkdtemp()
    # add a network -> action 'wifi' (caller reboots into RUN mode)
    assert portal.apply_save({"ssid": "Home", "psk": "secret", "priority": "3"}, d) == "wifi"
    nets = config.load_wifi(d)
    assert nets and nets[0]["ssid"] == "Home" and nets[0]["priority"] == 3

    # set character id only -> action 'device'
    assert portal.apply_save({"character_id": "999"}, d) == "device"
    assert config.load_device(d).character_id == "999"

    # brightness clamped to [0,1]
    portal.apply_save({"brightness": "5"}, d)
    assert config.load_device(d).brightness == 1.0

    # remove the network -> action 'removed'
    assert portal.apply_save({"remove_ssid": "Home"}, d) == "removed"
    assert config.load_wifi(d) == []

    # nothing actionable
    assert portal.apply_save({}, d) == ""


def test_render_page():
    d = tempfile.mkdtemp()
    portal.apply_save({"ssid": "Cafe", "psk": "x", "priority": "1"}, d)
    html = portal.render_page(d, ssids=["Cafe", "Home"], message="Saved.")
    assert "character_id" in html and "name=psk" in html
    assert "Cafe" in html and "Saved." in html
    assert "<option value=\"Home\">" in html


def main():
    test_dns_response()
    test_parse_form()
    test_apply_save_roundtrip()
    test_render_page()
    print("test_portal: OK")


if __name__ == "__main__":
    main()
