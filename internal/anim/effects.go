package anim

import (
	"math"

	"github.com/will/dnd-health-tracker/internal/config"
	"github.com/will/dnd-health-tracker/internal/led"
)

// This file holds the discrete, individually-testable animation primitives.
// Each is a pure function of its inputs (notably t, seconds since start) so it
// can be unit-tested with golden frames and previewed in isolation. The Engine
// (engine.go) composes them into the live display.

// ShimmerFactor returns an ambient "breathing" brightness multiplier in
// [1-amp, 1], oscillating at hz. With amp=0 it is a constant 1 (no effect).
func ShimmerFactor(amp, hz, t float64) float64 {
	if amp <= 0 || hz <= 0 {
		return 1
	}
	s := math.Sin(2 * math.Pi * hz * t) // -1..1
	return 1 - amp*0.5*(1-s)            // s=1 -> 1, s=-1 -> 1-amp
}

// PulseFactor returns a 0..1 heartbeat used to modulate brightness when near
// death. Phase chosen so t=0 is mid-brightness and rising.
func PulseFactor(hz, t float64) float64 {
	if hz <= 0 {
		return 1
	}
	return 0.5 * (1 + math.Sin(2*math.Pi*hz*t))
}

// Idle renders the resting display: the HP bar, dimmed by ambient shimmer, and
// — when at/below the low threshold — modulated by a low-HP heartbeat pulse.
func Idle(dst led.Frame, t config.Theme, frac, temp, secs float64) {
	RenderBar(dst, t, frac, temp)
	f := ShimmerFactor(t.IdleShimmer, t.IdleShimmerHz, secs)
	if frac <= t.LowFraction {
		// Pulse between ~45% and 100% brightness as a danger heartbeat.
		f *= 0.45 + 0.55*PulseFactor(t.LowPulseHz, secs)
	}
	scaleFrame(dst, f)
}

// FlashOverlay blends every pixel toward c by intensity (0..1). Used for the
// brief red/green hit/heal flash before the bar animates to its new level.
func FlashOverlay(dst led.Frame, c led.Color, intensity float64) {
	intensity = clamp01(intensity)
	for i := range dst {
		dst[i] = led.Lerp(dst[i], c, intensity)
	}
}

// BootSweep fills the strip up to progress (0..1) in the high-HP color: a quick
// "powering up" wipe shown at startup.
func BootSweep(dst led.Frame, t config.Theme, progress float64) {
	dst.Clear()
	n := len(dst)
	lit := int(clamp01(progress)*float64(n) + 0.5)
	col := HexC(t.HPHigh).Scale(clamp01(t.Brightness))
	for i := 0; i < lit && i < n; i++ {
		dst[i] = col
	}
}

// StatusPattern draws a non-HP status indicator:
//   - Connecting: a single dot scanning back and forth in the status color.
//   - Offline:    the whole strip breathing slowly in the status color.
//
// Online/Boot are handled elsewhere; for them this clears the strip.
func StatusPattern(dst led.Frame, t config.Theme, s Status, secs float64) {
	dst.Clear()
	n := len(dst)
	if n == 0 {
		return
	}
	b := clamp01(t.Brightness)
	col := HexC(t.Status)
	switch s {
	case StatusConnecting:
		// Triangle-wave scan position across the strip (~1.2 Hz sweep).
		phase := math.Mod(secs*1.2, 1.0)
		pos := int((1 - math.Abs(2*phase-1)) * float64(n-1))
		if pos >= 0 && pos < n {
			dst[pos] = col.Scale(b)
		}
	case StatusOffline:
		f := 0.2 + 0.8*PulseFactor(0.4, secs)
		dst.Fill(col.Scale(b * f))
	}
}

// scaleFrame multiplies every pixel's brightness by f.
func scaleFrame(dst led.Frame, f float64) {
	for i := range dst {
		dst[i] = dst[i].Scale(f)
	}
}
