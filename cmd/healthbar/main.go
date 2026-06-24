// Command healthbar drives the D&D Beyond LED health bar.
//
// Phase 1 wiring: load config, open the LED strip (terminal simulator by
// default), and render a health bar. With --demo it ramps HP up and down so
// the simulator output is visible without any D&D Beyond connection. Later
// phases add the DDB poller/websocket, animation state machine, and web UI.
package main

import (
	"flag"
	"log"
	"math"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/will/dnd-health-tracker/internal/anim"
	"github.com/will/dnd-health-tracker/internal/config"
	"github.com/will/dnd-health-tracker/internal/led"
)

func main() {
	var (
		configDir = flag.String("config-dir", "/etc/healthbar", "directory containing device.toml and theme.toml")
		sim       = flag.Bool("sim", true, "render to the terminal simulator instead of hardware")
		demo      = flag.Bool("demo", false, "animate HP up and down (no D&D Beyond connection)")
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

	// Clean shutdown: blank the strip on Ctrl-C / SIGTERM.
	sigs := make(chan os.Signal, 1)
	signal.Notify(sigs, syscall.SIGINT, syscall.SIGTERM)

	frame := led.NewFrame(strip.Len())

	if !*demo {
		anim.RenderBar(frame, cfg.Theme, *hp, *temp)
		if err := strip.Render(frame); err != nil {
			log.Fatalf("render: %v", err)
		}
		<-sigs
		return
	}

	runDemo(strip, frame, cfg, sigs)
}

// runDemo sweeps HP between full and empty so the bar mapping is visible in the
// simulator. Replaced by the real state machine in Phase 2.
func runDemo(strip led.Strip, frame led.Frame, cfg config.Config, sigs <-chan os.Signal) {
	fps := cfg.Theme.FPS
	ticker := time.NewTicker(time.Second / time.Duration(fps))
	defer ticker.Stop()

	start := time.Now()
	for {
		select {
		case <-sigs:
			return
		case <-ticker.C:
			// Triangle wave 0..1 over a 6-second period.
			phase := math.Mod(time.Since(start).Seconds()/6.0, 1.0)
			frac := 1 - math.Abs(2*phase-1)
			anim.RenderBar(frame, cfg.Theme, frac, 0)
			if err := strip.Render(frame); err != nil {
				log.Printf("render: %v", err)
				return
			}
		}
	}
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
