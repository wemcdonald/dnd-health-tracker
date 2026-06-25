"""Tests for the streaming HP extractor (run under CPython).

    cd firmware && python3 tests/test_hp.py

Validates the scanner against (a) hardcoded expected values, (b) an independent
full-parse reference, and (c) identical results when fed in tiny chunks (proving
tokens that span chunk boundaries are handled).
"""

import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import hp  # noqa: E402

FIXTURE = os.path.join(os.path.dirname(__file__), "fixture_character.json")


def reference_hp(doc):
    """Independent HP computation from a fully-parsed dict (oracle for the scanner)."""
    d = doc["data"]
    level = sum(c.get("level", 0) for c in d.get("classes", []))

    def stat_val(key):
        for s in d.get(key, []):
            if s.get("id") == hp.CON_STAT_ID:
                return s.get("value")
        return None

    con = 10
    if stat_val("stats") is not None:
        con = stat_val("stats")
    if stat_val("overrideStats") is not None:
        con = stat_val("overrideStats")
    if stat_val("bonusStats") is not None:
        con += stat_val("bonusStats")
    for arr in d.get("modifiers", {}).values():
        for m in arr:
            if m.get("type") == "bonus" and m.get("subType") == "constitution-score" \
                    and isinstance(m.get("value"), int):
                con += m["value"]
    return hp.compute_hp(
        d.get("baseHitPoints", 0) or 0,
        d.get("bonusHitPoints", 0) or 0,
        d.get("overrideHitPoints"),
        d.get("removedHitPoints", 0) or 0,
        d.get("temporaryHitPoints", 0) or 0,
        con,
        level,
    )


def chunked_parse(data, size):
    sc = hp._Scanner()
    for i in range(0, len(data), size):
        sc.feed(data[i:i + size])
    return sc.finalize()


def main():
    with open(FIXTURE, "rb") as f:
        raw = f.read()
    doc = json.loads(raw)

    # (a) hardcoded: con = 14 (stat) + 2 (bonusStat) + 1 (race modifier) = 17 ->
    # mod = (17-10)//2 = 3; level = 5; max = 40 + 8 + 3*5 = 63; cur = 63-13 = 50; temp = 9.
    got = hp.parse_hp_bytes(raw)
    assert got == (50, 63, 9), "whole-body parse: %r != (50, 63, 9)" % (got,)

    # (b) matches the independent oracle
    ref = reference_hp(doc)
    assert got == ref, "scanner %r != reference %r" % (got, ref)

    # (c) identical when fed in tiny chunks (token boundary stress)
    for size in (1, 2, 3, 7, 13, 64, 4096):
        c = chunked_parse(raw, size)
        assert c == got, "chunk size %d: %r != %r" % (size, c, got)

    # override path: max = override; stats/level irrelevant; early-exit allowed.
    ov = b'{"success":true,"data":{"overrideHitPoints":30,"removedHitPoints":5,"temporaryHitPoints":4,"baseHitPoints":1,"classes":[{"level":20}]}}'
    assert hp.parse_hp_bytes(ov) == (25, 30, 4), hp.parse_hp_bytes(ov)

    # negative CON modifier floors toward -inf: CON 7 -> (7-10)//2 = -2.
    assert hp.compute_hp(20, 0, None, 0, 0, 7, 1) == (18, 18, 0)
    # max floored to >= 1 and current clamped to [0, max].
    assert hp.compute_hp(0, 0, None, 100, 0, 10, 0) == (0, 1, 0)

    # private sheet (data: null) raises rather than returning bogus HP.
    try:
        hp.parse_hp_bytes(b'{"success":false,"message":"not found","data":null}')
        raise AssertionError("expected ValueError for null data")
    except ValueError:
        pass

    print("test_hp: OK  parsed=(cur=%d, max=%d, temp=%d)  ref=%r" % (got[0], got[1], got[2], ref))


if __name__ == "__main__":
    main()
