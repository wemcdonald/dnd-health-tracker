"""Tests for the animation engine + LED sim backend (run under CPython).

    cd firmware && python3 tests/test_anim.py
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import anim  # noqa: E402
import colors  # noqa: E402
import config  # noqa: E402
import leds  # noqa: E402

THEME = config.load_theme(os.path.join(os.path.dirname(__file__), "..", "data"))


def test_gradient():
    assert anim.gradient_color(THEME, 1.0) == THEME.hp_high
    assert anim.gradient_color(THEME, 0.0) == THEME.hp_low
    # just above the low threshold blends toward mid (not pure low)
    mid_band = anim.gradient_color(THEME, (THEME.low_fraction + THEME.mid_fraction) / 2)
    assert mid_band != THEME.hp_low and mid_band != THEME.hp_high


def test_render_bar_fill():
    dst = anim.new_frame(10)
    anim.render_bar(dst, THEME, 0.5, 0.0)
    lit = sum(1 for c in dst if c != colors.BLACK)
    assert lit == 5, "half of 10 should be lit, got %d" % lit
    # temp HP overshield lights pixels beyond current HP
    dst2 = anim.new_frame(10)
    anim.render_bar(dst2, THEME, 0.5, 0.2)
    lit2 = sum(1 for c in dst2 if c != colors.BLACK)
    assert lit2 == 7, "5 HP + 2 temp should light 7, got %d" % lit2


def test_engine_lifecycle():
    e = anim.Engine(THEME, 16)
    dst = anim.new_frame(16)
    assert e.status == anim.BOOT
    # advance past boot -> auto-transition to connecting
    e.render(dst, 1.3)
    assert e.status == anim.CONNECTING
    # first health snapshot shows instantly (no flash, no animation)
    e.set_status(anim.ONLINE)
    e.set_health(anim.Health(30, 30, 0))
    assert e.disp_frac == 1.0 and not e.flash_active
    # taking damage triggers a flash and starts the bar adjustment
    e.set_health(anim.Health(9, 30, 0))
    assert e.flash_active and e.flash_color == THEME.hp_low
    assert e.adjust_active and e.target_frac == 9 / 30
    # render long enough for the adjustment to finish -> display reaches target
    for _ in range(60):
        e.render(dst, 0.05)
    assert abs(e.disp_frac - 9 / 30) < 1e-6 and not e.adjust_active


def test_sim_strip():
    s = leds.SimStrip(8)
    frame = anim.new_frame(8)
    anim.render_bar(frame, THEME, 1.0, 0.0)
    line = s.line(frame)
    assert line.count("\x1b[0m") == 8  # one reset per pixel
    assert isinstance(leds.make_strip(8, 18), leds.SimStrip)  # no hw under CPython


def main():
    test_gradient()
    test_render_bar_fill()
    test_engine_lifecycle()
    test_sim_strip()
    print("test_anim: OK")


if __name__ == "__main__":
    main()
