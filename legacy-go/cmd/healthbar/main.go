// Command healthbar drives the D&D Beyond LED health bar.
//
// It loads configuration, opens the LED strip (terminal simulator by default),
// runs the animation engine in a render loop, supervises the D&D Beyond
// poller/websocket via the app package, and serves the no-auth web config UI.
// With --demo it scripts a boot/hit/heal sequence and skips networking so the
// simulator output is visible with no configuration.
package main

import (
	"context"
	"flag"
	"log"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/will/dnd-health-tracker/internal/anim"
	"github.com/will/dnd-health-tracker/internal/app"
	"github.com/will/dnd-health-tracker/internal/config"
	"github.com/will/dnd-health-tracker/internal/led"
	"github.com/will/dnd-health-tracker/internal/netcfg"
	"github.com/will/dnd-health-tracker/internal/web"
)

func main() {
	var (
		configDir  = flag.String("config-dir", "/etc/healthbar", "directory with device.toml, theme.toml, wifi.toml, secrets.toml")
		sim        = flag.Bool("sim", true, "render to the terminal simulator instead of hardware")
		demo       = flag.Bool("demo", false, "script a boot/damage/heal sequence (no networking)")
		webAddr    = flag.String("web-addr", ":8080", "address for the config web UI (empty to disable)")
		manageWiFi = flag.Bool("manage-wifi", false, "manage WiFi via NetworkManager + captive-portal fallback (Pi only)")
		hp         = flag.Float64("hp", 1.0, "static HP fraction when no character is configured")
		temp       = flag.Float64("temp", 0.0, "temporary-HP fraction for the static display")
	)
	flag.Parse()

	cfg, err := config.Load(*configDir)
	if err != nil {
		log.Fatalf("config: %v", err)
	}
	secrets, err := config.LoadSecrets(*configDir)
	if err != nil {
		log.Fatalf("config: %v", err)
	}
	wifi, err := config.LoadWiFi(*configDir)
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

	if *demo {
		go demoScript(ctx, engine)
	} else {
		application := app.New(*configDir, engine, cfg, secrets, wifi)
		if *manageWiFi {
			startWiFi(ctx, application, wifi)
		}
		application.Start(ctx)
		if cfg.Device.CharacterID == "" {
			go staticHP(ctx, engine, *hp, *temp)
		}
		if *webAddr != "" {
			go serveWeb(ctx, application, *webAddr)
		}
	}

	renderLoop(ctx, strip, engine, cfg.Theme.FPS)
}

// startWiFi syncs known networks into NetworkManager, installs the sync hook so
// web edits re-sync, and starts the captive-portal connectivity watcher.
func startWiFi(ctx context.Context, application *app.App, wifi config.WiFi) {
	mgr := netcfg.NewManager()
	application.SetWiFiSync(func(w config.WiFi) error { return mgr.Sync(ctx, w) })
	if err := mgr.Sync(ctx, wifi); err != nil {
		log.Printf("wifi sync: %v", err)
	}
	watcher := &netcfg.Watcher{Mgr: mgr, Log: log.Printf}
	go watcher.Run(ctx)
}

// serveWeb runs the config UI until ctx is cancelled.
func serveWeb(ctx context.Context, backend web.Backend, addr string) {
	srv := &http.Server{Addr: addr, Handler: web.NewServer(backend).Handler()}
	go func() {
		<-ctx.Done()
		shutCtx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
		defer cancel()
		_ = srv.Shutdown(shutCtx)
	}()
	log.Printf("web config UI on %s", addr)
	if err := srv.ListenAndServe(); err != nil && err != http.ErrServerClosed {
		log.Printf("web server: %v", err)
	}
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

// staticHP shows a fixed HP level (from --hp/--temp) once boot finishes, for
// development when no character is configured.
func staticHP(ctx context.Context, e *anim.Engine, hp, temp float64) {
	if !sleepCtx(ctx, bootSettle()) {
		return
	}
	e.SetStatus(anim.StatusOnline)
	e.SetHealth(fracHealth(hp, temp))
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

func statusPtr(s anim.Status) *anim.Status { return &s }

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
