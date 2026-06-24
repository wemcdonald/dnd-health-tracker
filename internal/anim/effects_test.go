package anim

import (
	"testing"

	"github.com/will/dnd-health-tracker/internal/config"
	"github.com/will/dnd-health-tracker/internal/led"
)

func TestShimmerFactorRange(t *testing.T) {
	if got := ShimmerFactor(0, 1, 3.3); got != 1 {
		t.Errorf("amp=0 should be constant 1, got %v", got)
	}
	for _, secs := range []float64{0, 0.1, 0.5, 1, 2.7, 10} {
		f := ShimmerFactor(0.2, 0.5, secs)
		if f < 0.8-1e-9 || f > 1+1e-9 {
			t.Errorf("ShimmerFactor out of [0.8,1]: %v at %v", f, secs)
		}
	}
}

func TestPulseFactorRange(t *testing.T) {
	for _, secs := range []float64{0, 0.25, 0.5, 1, 3} {
		f := PulseFactor(1.4, secs)
		if f < -1e-9 || f > 1+1e-9 {
			t.Errorf("PulseFactor out of [0,1]: %v", f)
		}
	}
}

func TestBootSweep(t *testing.T) {
	th := config.DefaultTheme()
	f := led.NewFrame(10)

	BootSweep(f, th, 0)
	if litCount(f) != 0 {
		t.Errorf("progress 0 lights none, got %d", litCount(f))
	}
	BootSweep(f, th, 1)
	if litCount(f) != 10 {
		t.Errorf("progress 1 lights all, got %d", litCount(f))
	}
	BootSweep(f, th, 0.5)
	if litCount(f) != 5 {
		t.Errorf("progress 0.5 lights 5, got %d", litCount(f))
	}
}

func TestStatusPattern(t *testing.T) {
	th := config.DefaultTheme()
	f := led.NewFrame(16)

	StatusPattern(f, th, StatusConnecting, 0)
	if litCount(f) != 1 {
		t.Errorf("connecting shows a single dot, got %d", litCount(f))
	}

	StatusPattern(f, th, StatusOffline, 0)
	if litCount(f) != 16 {
		t.Errorf("offline breathes whole strip, got %d", litCount(f))
	}
}

func TestFlashOverlay(t *testing.T) {
	f := led.Frame{led.RGB(0, 0, 0), led.RGB(10, 20, 30)}
	FlashOverlay(f, led.RGB(255, 0, 0), 1)
	if f[0] != (led.RGB(255, 0, 0)) || f[1] != (led.RGB(255, 0, 0)) {
		t.Errorf("intensity 1 should set all to flash color, got %v", f)
	}

	f = led.Frame{led.RGB(10, 20, 30)}
	FlashOverlay(f, led.RGB(255, 0, 0), 0)
	if f[0] != (led.RGB(10, 20, 30)) {
		t.Errorf("intensity 0 leaves frame unchanged, got %v", f[0])
	}
}
