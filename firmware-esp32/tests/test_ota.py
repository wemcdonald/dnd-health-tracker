"""Tests for the OTA manifest parser (run under CPython).

    cd firmware-esp32 && python3 tests/test_ota.py

Only the pure manifest parsing is host-tested; the partition streaming needs
real ESP32 flash and is validated on-hardware.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import ota  # noqa: E402

SHA = "9dd859260da15bea0e6cbd32c39868a5e1b48f9fd5a9af1f7166e48c5343ffb2"


def test_parses_valid_manifest():
    m = ota.parse_manifest("2 432280\n%s\n/firmware/image.bin\n" % SHA)
    assert m == {"version": 2, "size": 432280, "sha256": SHA,
                 "image_path": "/firmware/image.bin"}


def test_bytes_manifest():
    m = ota.parse_manifest(("1 100\n%s\n/firmware/image.bin\n" % SHA).encode())
    assert m["version"] == 1 and m["size"] == 100


def test_rejects_short():
    assert ota.parse_manifest("1 100\n%s" % SHA) is None
    assert ota.parse_manifest("") is None
    assert ota.parse_manifest(None) is None


def test_rejects_bad_sha():
    assert ota.parse_manifest("1 100\nnothex\n/firmware/image.bin\n") is None
    assert ota.parse_manifest("1 100\n%s\n/firmware/image.bin\n" % (SHA[:-1])) is None  # 63 chars


def test_rejects_bad_size():
    assert ota.parse_manifest("1 0\n%s\n/firmware/image.bin\n" % SHA) is None
    assert ota.parse_manifest("1 notanint\n%s\n/firmware/image.bin\n" % SHA) is None


def test_rejects_relative_image_path():
    assert ota.parse_manifest("1 100\n%s\nimage.bin\n" % SHA) is None


if __name__ == "__main__":
    fns = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    for fn in fns:
        fn()
        print("ok", fn.__name__)
    print("PASS", len(fns), "tests")
