// Command healthbar drives the D&D Beyond LED health bar.
//
// Phase 1/2 wiring: load config, open the LED strip (terminal simulator by
// default), and run the animation engine in a render loop. With --demo it
// scripts a boot→connecting→online sequence plus damage/heal events so the
// simulator output is visible without any D&D Beyond connection. Later phases
// feed the engine from the DDB poller/websocket and add the web UI.
package main

import (
	"context"
	"flag"
	"log"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/will/dnd-health-tracker/internal/anim"
	"github.com/will/dnd-health-tracker/internal/config"
	"github.com/will/dnd-health-tracker/internal/ddb"
	"github.com/will/dnd-health-tracker/internal/led"
)

func main() {
	var (
		configDir = flag.String("config-dir", "/etc/healthbar", "directory containing device.toml and theme.toml")
		sim       = flag.Bool("sim", true, "render to the terminal simulator instead of hardware")
		demo      = flag.Bool("demo", false, "script a boot/damage/heal sequence (no D&D Beyond connection)")
		hp        = flag.Float64("hp", 1.0, "static HP fraction to display when not in --demo mode")
		temp      = flag.Float64("temp", 0.0, "temporary-HP fraction to display")
	)
	flag.Parse()

	cfg, err := config.Load(*configDir)
	if err != nil {
		log.Fatalf("config: %v", err)
	}

	strip, err := openStrip(*sim, cfg.Device)
	if err != nil {
		log.Fatalf("led: %v", err)
	}
	defer strip.Close()

	engine := anim.NewEngine(cfg.Theme, strip.Len())

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	stop := make(chan os.Signal, 1)
	signal.Notify(stop, syscall.SIGINT, syscall.SIGTERM)
	go func() { <-stop; cancel() }()

	switch {
	case *demo:
		go demoScript(ctx, engine)
	case cfg.Device.CharacterID != "":
		go runPoller(ctx, engine, cfg.Device)
	default:
		// No character configured: show the static --hp level after boot.
		go func() {
			if !sleepCtx(ctx, bootSettle()) {
				return
			}
			engine.SetStatus(anim.StatusOnline)
			engine.SetHealth(fracHealth(*hp, *temp))
		}()
	}

	renderLoop(ctx, strip, engine, cfg.Theme.FPS)
}

// runPoller fetches HP from D&D Beyond and drives the engine: each change
// updates health, and reachability updates the connection status.
func runPoller(ctx context.Context, e *anim.Engine, d config.Device) {
	client := ddb.NewClient()
	p := &ddb.Poller{
		Fetcher:     client,
		CharacterID: d.CharacterID,
		Fast:        d.PollFast(),
		Idle:        d.PollIdle(),
		OnHP: func(hp ddb.HP) {
			e.SetStatus(anim.StatusOnline)
			e.SetHealth(anim.Health{Cur: hp.Current, Max: hp.Max, Temp: hp.Temp})
		},
		OnStatus: func(online bool) {
			if !online {
				e.SetStatus(anim.StatusOffline)
			}
		},
	}
	p.Run(ctx)
}

// renderLoop ticks at fps, advancing the engine by the real elapsed time
// between frames and pushing each frame to the strip. It returns when ctx is
// cancelled; the deferred strip.Close blanks the strip.
func renderLoop(ctx context.Context, strip led.Strip, e *anim.Engine, fps int) {
	if fps <= 0 {
		fps = 60
	}
	ticker := time.NewTicker(time.Second / time.Duration(fps))
	defer ticker.Stop()

	frame := led.NewFrame(strip.Len())
	last := time.Now()
	for {
		select {
		case <-ctx.Done():
			return
		case now := <-ticker.C:
			dt := now.Sub(last)
			last = now
			e.Render(frame, dt)
			if err := strip.Render(frame); err != nil {
				log.Printf("render: %v", err)
				return
			}
		}
	}
}

// demoScript exercises the engine: boot finishes on its own, then we connect,
// take a couple of hits, heal, and drop to a dangerous low-HP heartbeat. It
// loops until ctx is cancelled.
func demoScript(ctx context.Context, e *anim.Engine) {
	steps := []struct {
		after  time.Duration
		status *anim.Status
		health *anim.Health
	}{
		{after: bootSettle(), status: statusPtr(anim.StatusConnecting)},
		{after: 1500 * time.Millisecond, status: statusPtr(anim.StatusOnline), health: &anim.Health{Cur: 45, Max: 45}},
		{after: 2 * time.Second, health: &anim.Health{Cur: 30, Max: 45}},          // big hit
		{after: 2 * time.Second, health: &anim.Health{Cur: 30, Max: 45, Temp: 8}}, // gain temp HP
		{after: 2 * time.Second, health: &anim.Health{Cur: 40, Max: 45}},          // heal
		{after: 2 * time.Second, health: &anim.Health{Cur: 8, Max: 45}},           // near death
		{after: 3 * time.Second, health: &anim.Health{Cur: 45, Max: 45}},          // full heal
	}
	for {
		for _, s := range steps {
			if !sleepCtx(ctx, s.after) {
				return
			}
			if s.status != nil {
				e.SetStatus(*s.status)
			}
			if s.health != nil {
				e.SetHealth(*s.health)
			}
		}
	}
}

// sleepCtx sleeps for d or until ctx is cancelled; returns false if cancelled.
func sleepCtx(ctx context.Context, d time.Duration) bool {
	t := time.NewTimer(d)
	defer t.Stop()
	select {
	case <-ctx.Done():
		return false
	case <-t.C:
		return true
	}
}

func statusPtr(s anim.Status) *anim.Status { return &s }

// bootSettle is how long to wait for the boot sweep to finish before showing
// real data.
func bootSettle() time.Duration { return 1400 * time.Millisecond }

// fracHealth builds a Health from fractions using a 1000-unit virtual max so
// the static --hp/--temp flags drive the same engine path as real HP.
func fracHealth(hp, temp float64) anim.Health {
	const max = 1000
	return anim.Health{Cur: int(hp * max), Max: max, Temp: int(temp * max)}
}

// openStrip returns the terminal simulator, or the real hardware strip when
// --sim is false. hardwareStrip is provided by strip_hw.go (built with
// "-tags hw") or strip_nohw.go (the default, which returns an error).
func openStrip(sim bool, d config.Device) (led.Strip, error) {
	if sim {
		return led.NewSimulator(d.NumLEDs), nil
	}
	return hardwareStrip(d)
}
