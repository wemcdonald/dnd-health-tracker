"""Animation engine: turns health state into LED frames.

Faithful port of the Go ``internal/anim`` package (bar.go + effects.go +
engine.go). A "frame" is a plain list of (r, g, b) tuples, index 0 the first
LED. All effects are pure functions of their inputs (notably ``secs``) so they
can be unit-tested with golden frames; the Engine composes them into the live
display and is safe to call from the render thread while another thread updates
health/status.
"""

import math

import colors

try:  # lock that works on both MicroPython (core1 thread) and CPython (tests)
    import _thread
    _new_lock = _thread.allocate_lock
except ImportError:
    import threading
    _new_lock = threading.Lock

# Lifecycle / connection status (what the bar shows when not displaying live HP).
BOOT = 0        # powering-up sweep, shown briefly at start
CONNECTING = 1  # looking for WiFi / D&D Beyond: scanning dot
ONLINE = 2      # live HP bar
OFFLINE = 3     # lost connection: slow breathing pattern

BOOT_DURATION = 1.2  # seconds


# ----- frame helpers -------------------------------------------------------

def new_frame(n):
    return [colors.BLACK] * n


def frame_fill(dst, c):
    for i in range(len(dst)):
        dst[i] = c


def frame_clear(dst):
    frame_fill(dst, colors.BLACK)


def _clamp01(f):
    if f < 0:
        return 0.0
    if f > 1:
        return 1.0
    return f


# ----- bar + gradient (bar.go) --------------------------------------------

def gradient_color(theme, frac):
    """Bar color for a health fraction (0..1) using the theme high/mid/low colors."""
    frac = _clamp01(frac)
    low, mid = theme.low_fraction, theme.mid_fraction
    if frac <= low or mid <= low:
        return theme.hp_low
    if frac < mid:
        return colors.lerp(theme.hp_low, theme.hp_mid, (frac - low) / (mid - low))
    if mid >= 1:
        return theme.hp_high
    return colors.lerp(theme.hp_mid, theme.hp_high, (frac - mid) / (1 - mid))


def render_bar(dst, theme, frac, temp_frac):
    """Draw the health bar: a colored gradient run (with a dimmed partial leading
    pixel) plus a temporary-HP overshield in the theme temp color. Clears first."""
    n = len(dst)
    frame_clear(dst)
    if n == 0:
        return
    b = _clamp01(theme.brightness)
    frac = _clamp01(frac)
    col = gradient_color(theme, frac)

    filled = frac * n
    full = int(filled)
    if full > n:
        full = n
    rem = filled - full
    for i in range(full):
        dst[i] = colors.scale(col, b)
    nxt = full
    if full < n and rem > 0:
        dst[full] = colors.scale(col, b * rem)
        nxt = full + 1

    tcount = int(_clamp01(temp_frac) * n + 0.5)
    i = 0
    while i < tcount and nxt + i < n:
        dst[nxt + i] = colors.scale(theme.temp, b)
        i += 1


# ----- effects (effects.go) ------------------------------------------------

def shimmer_factor(amp, hz, t):
    """Ambient breathing multiplier in [1-amp, 1] at hz. amp<=0 -> constant 1."""
    if amp <= 0 or hz <= 0:
        return 1.0
    s = math.sin(2 * math.pi * hz * t)
    return 1 - amp * 0.5 * (1 - s)


def pulse_factor(hz, t):
    """0..1 heartbeat used to modulate brightness near death."""
    if hz <= 0:
        return 1.0
    return 0.5 * (1 + math.sin(2 * math.pi * hz * t))


def idle(dst, theme, frac, temp, secs):
    """Resting display: HP bar dimmed by ambient shimmer, plus a low-HP heartbeat."""
    render_bar(dst, theme, frac, temp)
    f = shimmer_factor(theme.idle_shimmer, theme.idle_shimmer_hz, secs)
    if frac <= theme.low_fraction:
        f *= 0.45 + 0.55 * pulse_factor(theme.low_pulse_hz, secs)
    _scale_frame(dst, f)


def flash_overlay(dst, c, intensity):
    """Blend every pixel toward c by intensity (0..1): the hit/heal flash."""
    intensity = _clamp01(intensity)
    for i in range(len(dst)):
        dst[i] = colors.lerp(dst[i], c, intensity)


def boot_sweep(dst, theme, progress):
    """Fill the strip up to progress (0..1) in the high-HP color: startup wipe."""
    frame_clear(dst)
    n = len(dst)
    lit = int(_clamp01(progress) * n + 0.5)
    col = colors.scale(theme.hp_high, _clamp01(theme.brightness))
    for i in range(min(lit, n)):
        dst[i] = col


def status_pattern(dst, theme, status, secs):
    """Non-HP status indicator: scanning dot (connecting) / breathing (offline)."""
    frame_clear(dst)
    n = len(dst)
    if n == 0:
        return
    b = _clamp01(theme.brightness)
    col = theme.status
    if status == CONNECTING:
        phase = (secs * 1.2) % 1.0
        pos = int((1 - abs(2 * phase - 1)) * (n - 1))
        if 0 <= pos < n:
            dst[pos] = colors.scale(col, b)
    elif status == OFFLINE:
        f = 0.2 + 0.8 * pulse_factor(0.4, secs)
        frame_fill(dst, colors.scale(col, b * f))


def _scale_frame(dst, f):
    for i in range(len(dst)):
        dst[i] = colors.scale(dst[i], f)


def _lerp_float(a, b, t):
    return a + (b - a) * t


def _ease_out_cubic(t):
    t = _clamp01(t)
    u = 1 - t
    return 1 - u * u * u


# ----- health snapshot -----------------------------------------------------

class Health:
    def __init__(self, cur, mx, temp):
        self.cur, self.max, self.temp = cur, mx, temp

    def fraction(self):
        if self.max <= 0:
            return 0.0
        return _clamp01(self.cur / self.max)

    def temp_fraction(self):
        if self.max <= 0:
            return 0.0
        return _clamp01(self.temp / self.max)

    def __eq__(self, o):
        return isinstance(o, Health) and (self.cur, self.max, self.temp) == (o.cur, o.max, o.temp)


# ----- engine state machine (engine.go) -----------------------------------

class Engine:
    """Animation state machine. NewEngine starts in the boot state."""

    def __init__(self, theme, n):
        self._lock = _new_lock()
        self.theme = theme
        self.n = n
        self.status = BOOT
        self.have_health = False
        self.secs = 0.0
        self.disp_frac = self.disp_temp = 0.0
        self.target_frac = self.target_temp = 0.0
        self.adjust_from = self.temp_from = 0.0
        self.adjust_elapsed = 0.0
        self.adjust_active = False
        self.flash_color = colors.BLACK
        self.flash_elapsed = 0.0
        self.flash_active = False
        self.boot_elapsed = 0.0

    def set_theme(self, theme):
        self._lock.acquire()
        self.theme = theme
        self._lock.release()

    def set_status(self, s):
        self._lock.acquire()
        self.status = s
        self._lock.release()

    def set_health(self, h):
        """Apply a new HP snapshot. First snapshot shows instantly; later changes
        trigger a red (damage) or green (heal) flash and animate the bar."""
        self._lock.acquire()
        try:
            frac, temp = h.fraction(), h.temp_fraction()
            if not self.have_health:
                self.have_health = True
                self.disp_frac = self.target_frac = frac
                self.disp_temp = self.target_temp = temp
                return
            if frac == self.target_frac and temp == self.target_temp:
                return
            if frac < self.target_frac:
                self.flash_color = self.theme.hp_low  # took damage
                self._trigger_flash()
            elif frac > self.target_frac:
                self.flash_color = self.theme.hp_high  # healed
                self._trigger_flash()
            self.adjust_from, self.temp_from = self.disp_frac, self.disp_temp
            self.target_frac, self.target_temp = frac, temp
            self.adjust_elapsed = 0.0
            self.adjust_active = True
        finally:
            self._lock.release()

    def _trigger_flash(self):
        self.flash_elapsed = 0.0
        self.flash_active = True

    def render(self, dst, dt):
        """Advance all animations by dt seconds and draw the current frame into dst."""
        self._lock.acquire()
        try:
            self.secs += dt
            self._advance_adjust(dt)
            self._advance_flash(dt)

            if self.status == BOOT:
                self.boot_elapsed += dt
                boot_sweep(dst, self.theme, self.boot_elapsed / BOOT_DURATION)
                if self.boot_elapsed >= BOOT_DURATION:
                    self.status = CONNECTING
                return  # no flash during boot
            if self.status == ONLINE and self.have_health:
                idle(dst, self.theme, self.disp_frac, self.disp_temp, self.secs)
            else:
                status_pattern(dst, self.theme, self.status, self.secs)

            if self.flash_active:
                intensity = 1 - self.flash_elapsed / self._flash_dur()
                flash_overlay(dst, self.flash_color, intensity)
        finally:
            self._lock.release()

    def _advance_adjust(self, dt):
        if not self.adjust_active:
            return
        self.adjust_elapsed += dt
        dur = self._adjust_dur()
        if dur <= 0 or self.adjust_elapsed >= dur:
            self.disp_frac, self.disp_temp = self.target_frac, self.target_temp
            self.adjust_active = False
            return
        p = _ease_out_cubic(self.adjust_elapsed / dur)
        self.disp_frac = _lerp_float(self.adjust_from, self.target_frac, p)
        self.disp_temp = _lerp_float(self.temp_from, self.target_temp, p)

    def _advance_flash(self, dt):
        if not self.flash_active:
            return
        self.flash_elapsed += dt
        if self.flash_elapsed >= self._flash_dur():
            self.flash_active = False

    def _flash_dur(self):
        if self.theme.flash_millis <= 0:
            return 0.001
        return self.theme.flash_millis / 1000

    def _adjust_dur(self):
        return self.theme.adjust_millis / 1000
