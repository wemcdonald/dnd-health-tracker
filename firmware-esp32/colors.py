"""24-bit RGB color helpers.

Ported from the Go ``internal/led`` Color type and ``internal/config`` HexColor.
A color is a plain ``(r, g, b)`` tuple of ints in 0..255 so it costs nothing on
the microcontroller and interoperates with MicroPython's ``neopixel`` module
(which takes ``(r, g, b)`` tuples and handles WS2812B GRB ordering itself).

Pure Python: runs unchanged under CPython for the desktop tests.
"""

BLACK = (0, 0, 0)


def rgb(r, g, b):
    return (int(r) & 0xFF, int(g) & 0xFF, int(b) & 0xFF)


def _clamp01(f):
    if f < 0:
        return 0.0
    if f > 1:
        return 1.0
    return f


def scale(c, f):
    """Multiply each channel by f (clamped to [0,1]) for brightness control."""
    f = _clamp01(f)
    return (int(c[0] * f), int(c[1] * f), int(c[2] * f))


def lerp(a, b, t):
    """Linearly interpolate between colors a and b. t clamped to [0,1]."""
    t = _clamp01(t)
    return (
        int(round(a[0] + (b[0] - a[0]) * t)),
        int(round(a[1] + (b[1] - a[1]) * t)),
        int(round(a[2] + (b[2] - a[2]) * t)),
    )


def add(c, o):
    """Saturating per-channel sum (used to overlay effects)."""
    return (min(255, c[0] + o[0]), min(255, c[1] + o[1]), min(255, c[2] + o[2]))


def from_hex(s):
    """Parse '#RRGGBB' (leading '#' optional) into an (r, g, b) tuple."""
    s = s.strip()
    if s.startswith("#"):
        s = s[1:]
    if len(s) != 6:
        raise ValueError("invalid hex color %r (want #RRGGBB)" % s)
    return (int(s[0:2], 16), int(s[2:4], 16), int(s[4:6], 16))


def to_hex(c):
    """Render an (r, g, b) tuple as '#RRGGBB'."""
    return "#%02x%02x%02x" % (c[0], c[1], c[2])
