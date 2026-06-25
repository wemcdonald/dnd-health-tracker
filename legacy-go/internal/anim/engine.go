package anim

import (
	"sync"
	"time"

	"github.com/will/dnd-health-tracker/internal/config"
	"github.com/will/dnd-health-tracker/internal/led"
)

// Status is the connection/lifecycle state that decides what the bar shows when
// it is not displaying live HP.
type Status int

const (
	StatusBoot       Status = iota // powering-up sweep, shown briefly at start
	StatusConnecting               // looking for WiFi / D&D Beyond: scanning dot
	StatusOnline                   // live HP bar
	StatusOffline                  // lost connection: slow breathing pattern
)

const bootDuration = 1200 * time.Millisecond

// Health is a snapshot of a character's hit points.
type Health struct {
	Cur, Max, Temp int
}

// Fraction is current HP as a fraction of max (0..1).
func (h Health) Fraction() float64 {
	if h.Max <= 0 {
		return 0
	}
	return clamp01(float64(h.Cur) / float64(h.Max))
}

// TempFraction is temporary HP as a fraction of max (0..1).
func (h Health) TempFraction() float64 {
	if h.Max <= 0 {
		return 0
	}
	return clamp01(float64(h.Temp) / float64(h.Max))
}

// Engine is the animation state machine. It is safe for concurrent use: the
// render goroutine calls Render while other goroutines call SetHealth/SetStatus.
type Engine struct {
	mu    sync.Mutex
	theme config.Theme
	n     int

	status     Status
	haveHealth bool
	secs       float64 // seconds accumulated, drives periodic effects

	// Eased bar level. displayed* track what is on screen; target* is the goal.
	dispFrac, dispTemp     float64
	targetFrac, targetTemp float64
	adjustFrom, tempFrom   float64
	adjustElapsed          float64
	adjustActive           bool

	// Hit/heal flash overlay.
	flashColor   led.Color
	flashElapsed float64
	flashActive  bool

	bootElapsed float64
}

// NewEngine creates an engine for an n-pixel strip starting in the boot state.
func NewEngine(t config.Theme, n int) *Engine {
	return &Engine{theme: t, n: n, status: StatusBoot}
}

// SetTheme swaps the theme live (used for hot-reload from the web UI / file).
func (e *Engine) SetTheme(t config.Theme) {
	e.mu.Lock()
	e.theme = t
	e.mu.Unlock()
}

// SetStatus updates the lifecycle state.
func (e *Engine) SetStatus(s Status) {
	e.mu.Lock()
	e.status = s
	e.mu.Unlock()
}

// SetHealth applies a new HP snapshot. The first snapshot is shown instantly;
// later changes trigger a red (damage) or green (heal) flash and animate the
// bar to its new level.
func (e *Engine) SetHealth(h Health) {
	e.mu.Lock()
	defer e.mu.Unlock()

	frac, temp := h.Fraction(), h.TempFraction()
	if !e.haveHealth {
		e.haveHealth = true
		e.dispFrac, e.targetFrac = frac, frac
		e.dispTemp, e.targetTemp = temp, temp
		return
	}
	if frac == e.targetFrac && temp == e.targetTemp {
		return // no change
	}
	if frac < e.targetFrac {
		e.flashColor = HexC(e.theme.HPLow) // took damage
		e.triggerFlash()
	} else if frac > e.targetFrac {
		e.flashColor = HexC(e.theme.HPHigh) // healed
		e.triggerFlash()
	}
	e.adjustFrom, e.tempFrom = e.dispFrac, e.dispTemp
	e.targetFrac, e.targetTemp = frac, temp
	e.adjustElapsed = 0
	e.adjustActive = true
}

func (e *Engine) triggerFlash() {
	e.flashElapsed = 0
	e.flashActive = true
}

// Render advances all animations by dt and draws the current frame into dst.
func (e *Engine) Render(dst led.Frame, dt time.Duration) {
	e.mu.Lock()
	defer e.mu.Unlock()

	sec := dt.Seconds()
	e.secs += sec
	e.advanceAdjust(sec)
	e.advanceFlash(sec)

	switch {
	case e.status == StatusBoot:
		e.bootElapsed += sec
		BootSweep(dst, e.theme, e.bootElapsed/bootDuration.Seconds())
		if e.bootElapsed >= bootDuration.Seconds() {
			e.status = StatusConnecting
		}
		return // no flash during boot
	case e.status == StatusOnline && e.haveHealth:
		Idle(dst, e.theme, e.dispFrac, e.dispTemp, e.secs)
	default:
		StatusPattern(dst, e.theme, e.status, e.secs)
	}

	if e.flashActive {
		intensity := 1 - e.flashElapsed/e.flashDur()
		FlashOverlay(dst, e.flashColor, intensity)
	}
}

func (e *Engine) advanceAdjust(sec float64) {
	if !e.adjustActive {
		return
	}
	e.adjustElapsed += sec
	dur := e.adjustDur()
	if dur <= 0 || e.adjustElapsed >= dur {
		e.dispFrac, e.dispTemp = e.targetFrac, e.targetTemp
		e.adjustActive = false
		return
	}
	p := easeOutCubic(e.adjustElapsed / dur)
	e.dispFrac = lerpFloat(e.adjustFrom, e.targetFrac, p)
	e.dispTemp = lerpFloat(e.tempFrom, e.targetTemp, p)
}

func (e *Engine) advanceFlash(sec float64) {
	if !e.flashActive {
		return
	}
	e.flashElapsed += sec
	if e.flashElapsed >= e.flashDur() {
		e.flashActive = false
	}
}

func (e *Engine) flashDur() float64 {
	if e.theme.FlashMillis <= 0 {
		return 0.001
	}
	return float64(e.theme.FlashMillis) / 1000
}

func (e *Engine) adjustDur() float64 {
	return float64(e.theme.AdjustMillis) / 1000
}

func lerpFloat(a, b, t float64) float64 { return a + (b-a)*t }

// easeOutCubic decelerates toward the end for a natural-looking bar adjustment.
func easeOutCubic(t float64) float64 {
	t = clamp01(t)
	u := 1 - t
	return 1 - u*u*u
}
