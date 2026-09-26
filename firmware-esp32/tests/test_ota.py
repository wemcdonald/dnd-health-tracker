"""Tests for the OTA manifest parser and rollback marker (run under CPython).

    cd firmware-esp32 && python3 tests/test_ota.py

The manifest parsing and the /data/ota.json marker are host-tested; the
partition streaming needs real ESP32 flash and is validated on-hardware.
"""

import os
import sys
import tempfile

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


# ----- /data/ota.json marker: skip a version that rolled back twice -------

def test_parse_state_keeps_only_int_versions():
    assert ota.parse_state({"pending": 3}) == {"pending": 3}
    assert ota.parse_state({"failed": 3, "attempts": 2}) == {"failed": 3, "attempts": 2}
    assert ota.parse_state({"pending": "3", "failed": True}) == {}
    assert ota.parse_state({"pending": -1}) == {}
    assert ota.parse_state([]) == {}
    assert ota.parse_state(None) == {}


def test_parse_state_old_marker_without_attempts_counts_as_one():
    assert ota.parse_state({"failed": 3}) == {"failed": 3, "attempts": 1}
    assert ota.parse_state({"failed": 3, "attempts": "x"}) == {"failed": 3, "attempts": 1}
    assert ota.parse_state({"failed": 3, "attempts": 0}) == {"failed": 3, "attempts": 1}


def test_parse_state_drops_attempts_without_failed():
    assert ota.parse_state({"attempts": 2}) == {}


def test_first_rollback_records_one_attempt():
    # v3 was staged, but we're running v2 again: the bootloader rolled back.
    assert ota.state_on_boot({"pending": 3}, running=2) == {"failed": 3, "attempts": 1}


def test_second_rollback_of_same_version_counts_up():
    s = {"pending": 3, "failed": 3, "attempts": 1}
    assert ota.state_on_boot(s, running=2) == {"failed": 3, "attempts": 2}


def test_rollback_of_a_different_version_restarts_the_count():
    s = {"pending": 4, "failed": 3, "attempts": 2}
    assert ota.state_on_boot(s, running=2) == {"failed": 4, "attempts": 1}


def test_boot_of_the_pending_image_keeps_it_pending():
    # v3 booted but hasn't reached the server yet: it can still roll back, so
    # the marker must survive until the image is confirmed.
    s = {"pending": 3, "failed": 3, "attempts": 1}
    assert ota.state_on_boot(s, running=3) == s


def test_boot_without_pending_is_unchanged():
    assert ota.state_on_boot({}, running=2) == {}
    s = {"failed": 3, "attempts": 1}
    assert ota.state_on_boot(s, running=2) == s


def test_staging_keeps_failed_info():
    s = {"failed": 3, "attempts": 1}
    assert ota.state_on_stage(s, 3) == {"pending": 3, "failed": 3, "attempts": 1}
    assert ota.state_on_stage(s, 4) == {"pending": 4, "failed": 3, "attempts": 1}
    assert s == {"failed": 3, "attempts": 1}  # not mutated


def test_confirm_clears_the_marker():
    assert ota.state_on_confirm({"pending": 3}, running=3) == {}
    # a retry that took clears its failure record too
    assert ota.state_on_confirm({"pending": 3, "failed": 3, "attempts": 1}, running=3) == {}


def test_confirm_without_pending_is_unchanged():
    assert ota.state_on_confirm({}, running=2) == {}
    s = {"failed": 3, "attempts": 1}
    assert ota.state_on_confirm(s, running=2) == s


def test_available_only_when_newer():
    assert ota.is_available(3, running=2, state={})
    assert not ota.is_available(2, running=2, state={})
    assert not ota.is_available(1, running=2, state={})


def test_version_that_failed_once_is_retried():
    assert ota.is_available(3, running=2, state={"failed": 3, "attempts": 1})


def test_version_that_failed_max_attempts_is_skipped():
    assert ota.OTA_MAX_ATTEMPTS == 2
    assert not ota.is_available(3, running=2, state={"failed": 3, "attempts": 2})
    assert not ota.is_available(3, running=2, state={"failed": 3, "attempts": 5})


def test_a_different_newer_version_clears_the_skip():
    assert ota.is_available(4, running=2, state={"failed": 3, "attempts": 2})


def _state_file(d):
    return os.path.join(d, "ota.json")


def test_full_rollback_cycle_on_disk():
    with tempfile.TemporaryDirectory() as d:
        assert ota.load_state(d) == {}                 # missing file
        ota.note_staged(d, 3)                          # v2 staged v3
        assert ota.load_state(d) == {"pending": 3}
        ota.note_boot(d, running=2)                    # rolled back to v2
        assert ota.load_state(d) == {"failed": 3, "attempts": 1}
        assert ota.is_available(3, 2, ota.load_state(d))   # one retry
        ota.note_staged(d, 3)
        ota.note_boot(d, running=2)                    # rolled back again
        assert ota.load_state(d) == {"failed": 3, "attempts": 2}
        assert not ota.is_available(3, 2, ota.load_state(d))
        ota.note_staged(d, 4)                          # v4 published and staged
        ota.note_boot(d, running=4)
        assert ota.load_state(d) == {"pending": 4, "failed": 3, "attempts": 2}
        ota.note_confirmed(d, running=4)
        assert ota.load_state(d) == {}


def test_retry_that_takes_clears_the_marker_on_disk():
    with tempfile.TemporaryDirectory() as d:
        ota.note_staged(d, 3)
        ota.note_boot(d, running=2)                    # transient failure
        ota.note_staged(d, 3)                          # retry
        ota.note_boot(d, running=3)
        ota.note_confirmed(d, running=3)
        assert ota.load_state(d) == {}


def test_note_boot_does_not_write_when_unchanged():
    with tempfile.TemporaryDirectory() as d:
        ota.note_boot(d, running=2)
        ota.note_confirmed(d, running=2)
        assert not os.path.exists(_state_file(d))  # no flash write on a normal boot


def test_marker_io_never_raises():
    with tempfile.TemporaryDirectory() as d:
        bad = d + "/no/such/dir"  # single-level mkdir can't create this
        ota.note_staged(bad, 3)
        ota.note_boot(bad, running=2)
        ota.note_confirmed(bad, running=2)
        assert ota.load_state(bad) == {}
        with open(_state_file(d), "w") as f:
            f.write("{not json")
        assert ota.load_state(d) == {}


# ----- apply_update ordering (streaming stubbed out) -----------------------

class _FakePart:
    def __init__(self, fail):
        self.fail = fail
        self.booted = False

    def set_boot(self):
        if self.fail:
            raise OSError("set_boot failed")
        self.booted = True


def _apply_with(part, d):
    real = ota._stream_image_to_partition
    ota._stream_image_to_partition = lambda dev, m: part
    try:
        return ota.apply_update(None, {"version": 3}, d)
    finally:
        ota._stream_image_to_partition = real


def test_apply_update_records_pending_after_set_boot():
    with tempfile.TemporaryDirectory() as d:
        part = _FakePart(fail=False)
        assert _apply_with(part, d) is True
        assert part.booted and ota.load_state(d) == {"pending": 3}


def test_apply_update_set_boot_failure_leaves_no_marker():
    with tempfile.TemporaryDirectory() as d:
        assert _apply_with(_FakePart(fail=True), d) is False
        assert not os.path.exists(_state_file(d))


def test_apply_update_stream_failure_leaves_no_marker():
    with tempfile.TemporaryDirectory() as d:
        assert _apply_with(None, d) is False
        assert not os.path.exists(_state_file(d))


if __name__ == "__main__":
    fns = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    for fn in fns:
        fn()
        print("ok", fn.__name__)
    print("PASS", len(fns), "tests")
