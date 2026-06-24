package netcfg

import (
	"context"
	"time"
)

// Watcher monitors connectivity and runs the captive-portal access point when
// no known network is reachable, periodically dropping it to retry known nets.
type Watcher struct {
	Mgr           *Manager
	CheckInterval time.Duration // how often to check connectivity
	OfflineGrace  time.Duration // stay offline this long before starting the AP
	HotspotPeriod time.Duration // keep the AP up this long before retrying nets
	Log           func(format string, args ...any)
}

// Run watches until ctx is cancelled. Blocking; run in a goroutine.
func (w *Watcher) Run(ctx context.Context) {
	if w.CheckInterval <= 0 {
		w.CheckInterval = 15 * time.Second
	}
	if w.OfflineGrace <= 0 {
		w.OfflineGrace = 45 * time.Second
	}
	if w.HotspotPeriod <= 0 {
		w.HotspotPeriod = 90 * time.Second
	}

	var offlineSince, hotspotSince time.Time
	for {
		if !sleepCtx(ctx, w.CheckInterval) {
			return
		}
		switch {
		case w.Mgr.HotspotActive():
			if time.Since(hotspotSince) >= w.HotspotPeriod {
				w.log("retrying known networks")
				_ = w.Mgr.StopHotspot(ctx)
				_, _ = w.Mgr.Runner.Run(ctx, "nmcli", "device", "wifi", "rescan")
				offlineSince = time.Time{}
			}
		case w.Mgr.Connectivity(ctx):
			offlineSince = time.Time{}
		case offlineSince.IsZero():
			offlineSince = time.Now()
		case time.Since(offlineSince) >= w.OfflineGrace:
			w.log("no known network reachable; starting setup hotspot %q", w.Mgr.HotspotSSID)
			if err := w.Mgr.StartHotspot(ctx); err != nil {
				w.log("hotspot start failed: %v", err)
			} else {
				hotspotSince = time.Now()
			}
		}
	}
}

func (w *Watcher) log(format string, args ...any) {
	if w.Log != nil {
		w.Log(format, args...)
	}
}

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
