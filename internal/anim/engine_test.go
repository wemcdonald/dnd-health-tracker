package anim

import (
	"testing"
	"time"

	"github.com/will/dnd-health-tracker/internal/config"
	"github.com/will/dnd-health-tracker/internal/led"
)

func TestHealthFractions(t *testing.T) {
	h := Health{Cur: 30, Max: 45, Temp: 9}
	if got := h.Fraction(); got < 0.66 || got > 0.67 {
		t.Errorf("Fraction = %v, want ~0.667", got)
	}
	if got := h.TempFraction(); got < 0.19 || got > 0.21 {
		t.Errorf("TempFraction = %v, want ~0.2", got)
	}
	if (Health{Cur: 5, Max: 0}).Fraction() != 0 {
		t.Error("zero max should yield 0 fraction, not NaN")
	}
}

func TestEngineBootTransitionsToConnecting(t *testing.T) {
	e := NewEngine(config.DefaultTheme(), 8)
	f := led.NewFrame(8)

	e.Render(f, 100*time.Millisecond)
	if e.status != StatusBoot {
		t.Fatalf("should still be booting at 100ms, got %v", e.status)
	}
	if litCount(f) == 0 {
		t.Error("boot sweep should light some pixels")
	}
	// Advance past the boot duration.
	e.Render(f, bootDuration)
	if e.status != StatusConnecting {
		t.Errorf("after boot duration status should be Connecting, got %v", e.status)
	}
}

func TestEngineFirstHealthSnaps(t *testing.T) {
	e := NewEngine(config.DefaultTheme(), 16)
	e.SetHealth(Health{Cur: 20, Max: 40}) // 0.5
	if e.dispFrac != 0.5 || e.adjustActive || e.flashActive {
		t.Errorf("first health should snap with no flash/animation: disp=%v adjust=%v flash=%v",
			e.dispFrac, e.adjustActive, e.flashActive)
	}
}

func TestEngineDamageFlashesAndEases(t *testing.T) {
	th := config.DefaultTheme()
	th.AdjustMillis = 600
	e := NewEngine(th, 16)
	e.SetHealth(Health{Cur: 40, Max: 40}) // snap to 1.0
	e.SetHealth(Health{Cur: 20, Max: 40}) // damage to 0.5

	if !e.flashActive {
		t.Error("damage should trigger a flash")
	}
	if e.flashColor != HexC(th.HPLow) {
		t.Errorf("damage flash should use HPLow color, got %v", e.flashColor)
	}
	if !e.adjustActive || e.dispFrac != 1.0 {
		t.Errorf("displayed should not have moved yet: disp=%v active=%v", e.dispFrac, e.adjustActive)
	}

	f := led.NewFrame(16)
	e.Render(f, 300*time.Millisecond) // halfway through the 600ms ease
	if e.dispFrac <= 0.5 || e.dispFrac >= 1.0 {
		t.Errorf("mid-ease displayed should be between 0.5 and 1.0, got %v", e.dispFrac)
	}
	e.Render(f, 400*time.Millisecond) // past the end
	if e.dispFrac != 0.5 || e.adjustActive {
		t.Errorf("ease should complete at target 0.5, got %v active=%v", e.dispFrac, e.adjustActive)
	}
}

func TestEngineHealUsesHighColor(t *testing.T) {
	th := config.DefaultTheme()
	e := NewEngine(th, 16)
	e.SetHealth(Health{Cur: 20, Max: 40}) // snap 0.5
	e.SetHealth(Health{Cur: 40, Max: 40}) // heal to 1.0
	if !e.flashActive || e.flashColor != HexC(th.HPHigh) {
		t.Errorf("heal should flash HPHigh, got active=%v color=%v", e.flashActive, e.flashColor)
	}
}

func TestEngineFlashExpires(t *testing.T) {
	th := config.DefaultTheme()
	th.FlashMillis = 200
	e := NewEngine(th, 16)
	e.SetHealth(Health{Cur: 40, Max: 40})
	e.SetHealth(Health{Cur: 10, Max: 40})
	f := led.NewFrame(16)
	e.Render(f, 250*time.Millisecond) // past the 200ms flash
	if e.flashActive {
		t.Error("flash should have expired")
	}
}
