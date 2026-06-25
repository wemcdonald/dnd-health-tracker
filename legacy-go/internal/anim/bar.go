// Package anim turns health state into LED frames: the static HP→bar mapping
// here, plus the time-based animations and state machine added in later phases.
package anim

import (
	"github.com/will/dnd-health-tracker/internal/config"
	"github.com/will/dnd-health-tracker/internal/led"
)

// GradientColor returns the bar color for a health fraction (0..1) using the
// theme's high/mid/low colors and thresholds.
func GradientColor(t config.Theme, frac float64) led.Color {
	frac = clamp01(frac)
	low, mid := t.LowFraction, t.MidFraction
	hi := HexC(t.HPHigh)
	md := HexC(t.HPMid)
	lo := HexC(t.HPLow)
	switch {
	case frac <= low || mid <= low:
		return lo
	case frac < mid:
		return led.Lerp(lo, md, (frac-low)/(mid-low))
	default:
		if mid >= 1 {
			return hi
		}
		return led.Lerp(md, hi, (frac-mid)/(1-mid))
	}
}

// RenderBar draws a health bar into dst: current HP as a colored gradient run
// (with a dimmed partial leading pixel for smoothness) and temporary HP as a
// distinct overshield in the theme's temp color. brightness scales the whole
// bar (theme render multiplier). dst is cleared first.
func RenderBar(dst led.Frame, t config.Theme, frac, tempFrac float64) {
	n := len(dst)
	dst.Clear()
	if n == 0 {
		return
	}
	b := clamp01(t.Brightness)
	frac = clamp01(frac)
	col := GradientColor(t, frac)

	filled := frac * float64(n)
	full := int(filled)
	if full > n {
		full = n
	}
	rem := filled - float64(full)
	for i := 0; i < full; i++ {
		dst[i] = col.Scale(b)
	}
	next := full
	if full < n && rem > 0 {
		dst[full] = col.Scale(b * rem)
		next = full + 1
	}

	// Temporary HP overshield occupies pixels just past current HP.
	tempCol := HexC(t.Temp)
	tcount := int(clamp01(tempFrac)*float64(n) + 0.5)
	for i := 0; i < tcount && next+i < n; i++ {
		dst[next+i] = tempCol.Scale(b)
	}
}

// HexC unwraps a config.HexColor to a led.Color.
func HexC(h config.HexColor) led.Color { return h.Color() }

func clamp01(f float64) float64 {
	if f < 0 {
		return 0
	}
	if f > 1 {
		return 1
	}
	return f
}
