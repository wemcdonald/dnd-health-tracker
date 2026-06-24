// Package app is the runtime supervisor that ties configuration, the D&D
// Beyond poller/websocket, and the animation engine together. It is the single
// place that applies configuration changes (from files or the web UI),
// restarting the D&D Beyond side and live-updating the engine theme as needed.
// It implements the backend the web package depends on.
package app

import (
	"context"
	"net"
	"sync"
	"time"

	"github.com/will/dnd-health-tracker/internal/anim"
	"github.com/will/dnd-health-tracker/internal/config"
	"github.com/will/dnd-health-tracker/internal/ddb"
)

// Status is a read-only snapshot of runtime state for display.
type Status struct {
	Player      string
	CharacterID string
	Connection  string // "online", "offline", or "connecting"
	HP          int
	MaxHP       int
	TempHP      int
	HasCookie   bool
	PublicMode  bool
	GameLinked  bool
	IPs         []string
	UpdatedAgo  time.Duration
}

// App supervises the running health bar.
type App struct {
	configDir string
	engine    *anim.Engine

	mu        sync.Mutex
	cfg       config.Config
	secrets   config.Secrets
	wifi      config.WiFi
	hp        ddb.HP
	haveHP    bool
	online    bool
	updatedAt time.Time

	rootCtx   context.Context
	cancelDDB context.CancelFunc
	wifiSync  WiFiSyncFunc
}

// New builds an App from already-loaded configuration.
func New(configDir string, engine *anim.Engine, cfg config.Config, secrets config.Secrets, wifi config.WiFi) *App {
	return &App{configDir: configDir, engine: engine, cfg: cfg, secrets: secrets, wifi: wifi}
}

// Start records the root context and launches the D&D Beyond side. It returns
// immediately; the poller/websocket run in background goroutines until the root
// context is cancelled.
func (a *App) Start(ctx context.Context) {
	a.mu.Lock()
	defer a.mu.Unlock()
	a.rootCtx = ctx
	a.startDDBLocked()
}

// startDDBLocked (re)starts the poller and optional websocket using the current
// config + secrets. Caller holds a.mu. A no-op if no character is configured.
func (a *App) startDDBLocked() {
	if a.cancelDDB != nil {
		a.cancelDDB()
		a.cancelDDB = nil
	}
	if a.rootCtx == nil || a.cfg.Device.CharacterID == "" {
		return
	}
	ctx, cancel := context.WithCancel(a.rootCtx)
	a.cancelDDB = cancel

	client := ddb.NewClient()
	var auth *ddb.CookieAuth
	if a.secrets.CobaltCookie != "" {
		auth = ddb.NewCookieAuth(a.secrets.CobaltCookie)
		client.SetAuthorizer(auth)
	}
	p := &ddb.Poller{
		Fetcher:     client,
		CharacterID: a.cfg.Device.CharacterID,
		Fast:        a.cfg.Device.PollFast(),
		Idle:        a.cfg.Device.PollIdle(),
		OnHP:        a.onHP,
		OnStatus:    a.onStatus,
	}
	if auth != nil && a.cfg.Device.GameID != "" && a.cfg.Device.UserID != "" {
		ws := ddb.NewWSListener(auth, a.cfg.Device.GameID, a.cfg.Device.UserID)
		ws.OnNudge = p.Nudge
		go ws.Run(ctx)
	}
	go p.Run(ctx)
}

func (a *App) onHP(hp ddb.HP) {
	a.mu.Lock()
	a.hp, a.haveHP, a.online, a.updatedAt = hp, true, true, time.Now()
	a.mu.Unlock()
	a.engine.SetStatus(anim.StatusOnline)
	a.engine.SetHealth(anim.Health{Cur: hp.Current, Max: hp.Max, Temp: hp.Temp})
}

func (a *App) onStatus(online bool) {
	a.mu.Lock()
	a.online = online
	a.mu.Unlock()
	if !online {
		a.engine.SetStatus(anim.StatusOffline)
	}
}

// Snapshot returns the current runtime status.
func (a *App) Snapshot() Status {
	a.mu.Lock()
	defer a.mu.Unlock()
	conn := "connecting"
	if a.online {
		conn = "online"
	} else if a.haveHP {
		conn = "offline"
	}
	var ago time.Duration
	if !a.updatedAt.IsZero() {
		ago = time.Since(a.updatedAt)
	}
	return Status{
		Player:      a.cfg.Device.PlayerName,
		CharacterID: a.cfg.Device.CharacterID,
		Connection:  conn,
		HP:          a.hp.Current,
		MaxHP:       a.hp.Max,
		TempHP:      a.hp.Temp,
		HasCookie:   a.secrets.CobaltCookie != "",
		PublicMode:  a.secrets.CobaltCookie == "",
		GameLinked:  a.cfg.Device.GameID != "" && a.cfg.Device.UserID != "",
		IPs:         localIPs(),
		UpdatedAgo:  ago,
	}
}

// localIPs returns the non-loopback IPv4 addresses, for showing how to reach
// the web UI.
func localIPs() []string {
	var out []string
	addrs, err := net.InterfaceAddrs()
	if err != nil {
		return out
	}
	for _, a := range addrs {
		if ipnet, ok := a.(*net.IPNet); ok && !ipnet.IP.IsLoopback() {
			if v4 := ipnet.IP.To4(); v4 != nil {
				out = append(out, v4.String())
			}
		}
	}
	return out
}
