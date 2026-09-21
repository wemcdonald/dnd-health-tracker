"""LED strip backends.

Replaces the Go ``internal/led`` Strip interface and its hw/sim backends:

  - NeoStrip : real WS2812B via MicroPython's ``neopixel`` (PIO-driven). The
    module emits GRB ordering for WS2812 itself, so we pass plain (r, g, b).
  - SimStrip : ANSI truecolor terminal renderer for desktop development.

``make_strip`` auto-selects NeoStrip on hardware and falls back to SimStrip
elsewhere (and under CPython). The device's hardware ``brightness`` (0..1) is
applied here on write, on top of the theme's render-time multiplier.
"""

import colors


class SimStrip:
    """Renders the strip as colored blocks on a terminal line (dev only)."""

    def __init__(self, n, brightness=1.0):
        self.n = n
        self.brightness = brightness

    def line(self, frame):
        out = []
        for c in frame:
            r, g, b = colors.scale(c, self.brightness)
            out.append("\x1b[48;2;%d;%d;%dm  \x1b[0m" % (r, g, b))
        return "".join(out)

    def render(self, frame):
        # \r keeps redrawing on one line so the animation plays in place.
        print("\r" + self.line(frame), end="")

    def __len__(self):
        return self.n

    def close(self):
        print()  # leave the cursor on a fresh line


class NeoStrip:
    """Drives a physical WS2812B strip on the given GPIO via neopixel/PIO."""

    def __init__(self, n, pin, brightness=1.0):
        import machine
        import neopixel
        self.n = n
        self.brightness = brightness
        self.np = neopixel.NeoPixel(machine.Pin(pin), n)

    def render(self, frame):
        b = self.brightness
        np = self.np
        for i in range(self.n):
            np[i] = colors.scale(frame[i], b)
        np.write()

    def __len__(self):
        return self.n

    def close(self):
        for i in range(self.n):
            self.np[i] = colors.BLACK
        self.np.write()


def make_strip(n, pin, brightness=1.0):
    """Return a hardware strip if available, else the terminal simulator."""
    try:
        import machine  # noqa: F401
        import neopixel  # noqa: F401
    except ImportError:
        return SimStrip(n, brightness)
    return NeoStrip(n, pin, brightness)
