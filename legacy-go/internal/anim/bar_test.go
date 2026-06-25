package anim

import (
	"testing"

	"github.com/will/dnd-health-tracker/internal/config"
	"github.com/will/dnd-health-tracker/internal/led"
)

func litCount(f led.Frame) int {
	n := 0
	for _, c := range f {
		if c != led.Black {
			n++
		}
	}
	return n
}

func TestRenderBarFullAndEmpty(t *testing.T) {
	th := config.DefaultTheme()
	f := led.NewFrame(16)

	RenderBar(f, th, 1.0, 0)
	if litCount(f) != 16 {
		t.Errorf("full HP should light all 16, got %d", litCount(f))
	}

	RenderBar(f, th, 0.0, 0)
	if litCount(f) != 0 {
		t.Errorf("zero HP should light none, got %d", litCount(f))
	}
}

func TestRenderBarHalf(t *testing.T) {
	th := config.DefaultTheme()
	f := led.NewFrame(16)
	RenderBar(f, th, 0.5, 0)
	if got := litCount(f); got != 8 {
		t.Errorf("half HP on 16 LEDs should light 8, got %d", got)
	}
}

func TestRenderBarPartialPixelDimmed(t *testing.T) {
	th := config.DefaultTheme()
	f := led.NewFrame(10)
	RenderBar(f, th, 0.55, 0) // 5.5 LEDs -> 5 full + 1 half-bright
	full := GradientColor(th, 0.55).Scale(th.Brightness)
	if f[4] != full {
		t.Errorf("pixel 4 should be full bright %v, got %v", full, f[4])
	}
	if f[5] == led.Black || f[5] == full {
		t.Errorf("pixel 5 should be a dimmed partial, got %v", f[5])
	}
}

func TestRenderBarTempOvershield(t *testing.T) {
	th := config.DefaultTheme()
	f := led.NewFrame(16)
	RenderBar(f, th, 0.5, 0.125) // 8 HP + 2 temp
	temp := HexC(th.Temp).Scale(th.Brightness)
	if f[8] != temp || f[9] != temp {
		t.Errorf("temp overshield should occupy pixels 8-9, got %v %v", f[8], f[9])
	}
	if litCount(f) != 10 {
		t.Errorf("8 HP + 2 temp should light 10, got %d", litCount(f))
	}
}

func TestGradientThresholds(t *testing.T) {
	th := config.DefaultTheme()
	if GradientColor(th, 1.0) != HexC(th.HPHigh) {
		t.Error("full HP should be high color")
	}
	if GradientColor(th, 0.1) != HexC(th.HPLow) {
		t.Error("below low threshold should be low color")
	}
}
