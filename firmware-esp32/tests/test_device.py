"""Tests for board identity (run under CPython).

    cd firmware-esp32 && python3 tests/test_device.py
"""

import json
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import device  # noqa: E402

HEX = "0123456789abcdef"


def test_format_mac():
    assert device.format_mac(bytes([0xAC, 0x27, 0x6E, 0x7D, 0x17, 0x74])) == "ac276e7d1774"


def test_new_token_is_32_lower_hex():
    t = device.new_token()
    assert len(t) == 32 and all(c in HEX for c in t)
    assert device.new_token() != t


def test_identity_created_once_and_persisted():
    with tempfile.TemporaryDirectory() as d:
        a = device.load_identity(d)
        b = device.load_identity(d)
        assert a["token"] == b["token"]
        assert a["config_rev"] == 0
        with open(d + "/device.json") as f:
            assert json.load(f)["token"] == a["token"]


def test_malformed_token_is_regenerated():
    with tempfile.TemporaryDirectory() as d:
        with open(d + "/device.json", "w") as f:
            json.dump({"token": "XYZ", "config_rev": 3}, f)
        ident = device.load_identity(d, token_factory=lambda: "b" * 32)
        assert ident["token"] == "b" * 32
        assert ident["config_rev"] == 3


def test_save_identity_roundtrip():
    with tempfile.TemporaryDirectory() as d:
        ident = device.load_identity(d, token_factory=lambda: "c" * 32)
        ident["config_rev"] = 5
        device.save_identity(ident, d)
        assert device.load_identity(d) == {"token": "c" * 32, "config_rev": 5}


def test_load_identity_creates_missing_data_dir():
    # Simulates a full flash erase: /data doesn't exist yet on first boot.
    with tempfile.TemporaryDirectory() as d:
        data_dir = d + "/data"
        assert not os.path.isdir(data_dir)
        ident = device.load_identity(data_dir, token_factory=lambda: "e" * 32)
        assert ident["token"] == "e" * 32
        with open(data_dir + "/device.json") as f:
            assert json.load(f)["token"] == "e" * 32


def test_load_identity_survives_unwritable_flash():
    # Parent directory doesn't exist and can't be created with a single-level
    # mkdir -> save fails, but the board must still boot with an in-memory identity.
    with tempfile.TemporaryDirectory() as d:
        data_dir = d + "/no/such/dir"
        ident = device.load_identity(data_dir, token_factory=lambda: "f" * 32)
        assert ident["token"] == "f" * 32
        assert ident["config_rev"] == 0
        assert not os.path.exists(data_dir + "/device.json")


if __name__ == "__main__":
    fns = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    for fn in fns:
        fn()
        print("ok", fn.__name__)
    print("PASS", len(fns), "tests")
