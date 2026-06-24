package app

import (
	"github.com/will/dnd-health-tracker/internal/config"
)

// ConfigDir returns the active configuration directory.
func (a *App) ConfigDir() string { return a.configDir }

// Device returns a copy of the current device config.
func (a *App) Device() config.Device {
	a.mu.Lock()
	defer a.mu.Unlock()
	return a.cfg.Device
}

// Theme returns a copy of the current theme.
func (a *App) Theme() config.Theme {
	a.mu.Lock()
	defer a.mu.Unlock()
	return a.cfg.Theme
}

// WiFiNetworks returns a copy of the known networks.
func (a *App) WiFiNetworks() []config.WiFiNetwork {
	a.mu.Lock()
	defer a.mu.Unlock()
	out := make([]config.WiFiNetwork, len(a.wifi.Networks))
	copy(out, a.wifi.Networks)
	return out
}

// SaveDevice persists device.toml and restarts the D&D Beyond side, since
// character id, IDs, or poll cadence may have changed. Hardware fields
// (num_leds, gpio_pin) take effect on the next process start.
func (a *App) SaveDevice(d config.Device) error {
	if err := config.SaveDevice(a.configDir, d); err != nil {
		return err
	}
	a.mu.Lock()
	a.cfg.Device = d
	a.startDDBLocked()
	a.mu.Unlock()
	return nil
}

// SaveTheme persists theme.toml and pushes it to the engine for a live update.
func (a *App) SaveTheme(t config.Theme) error {
	if err := config.SaveTheme(a.configDir, t); err != nil {
		return err
	}
	a.mu.Lock()
	a.cfg.Theme = t
	a.mu.Unlock()
	a.engine.SetTheme(t)
	return nil
}

// OnWiFiChange, if set, is invoked after the known-networks list changes so the
// OS network manager can be re-synced (wired up in Phase 6).
type WiFiSyncFunc func(config.WiFi) error

// SetWiFiSync installs the network-manager sync hook.
func (a *App) SetWiFiSync(f WiFiSyncFunc) {
	a.mu.Lock()
	a.wifiSync = f
	a.mu.Unlock()
}

// UpsertWiFi adds or updates a known network and persists the list.
func (a *App) UpsertWiFi(n config.WiFiNetwork) error {
	a.mu.Lock()
	a.wifi.Upsert(n)
	w := a.wifi
	sync := a.wifiSync
	a.mu.Unlock()
	return a.persistWiFi(w, sync)
}

// RemoveWiFi deletes a known network and persists the list.
func (a *App) RemoveWiFi(ssid string) error {
	a.mu.Lock()
	a.wifi.Remove(ssid)
	w := a.wifi
	sync := a.wifiSync
	a.mu.Unlock()
	return a.persistWiFi(w, sync)
}

func (a *App) persistWiFi(w config.WiFi, sync WiFiSyncFunc) error {
	if err := config.SaveWiFi(a.configDir, w); err != nil {
		return err
	}
	if sync != nil {
		return sync(w)
	}
	return nil
}

// Credentials carries the values captured from D&D Beyond (via the bookmarklet
// or manual paste). Empty fields are left unchanged.
type Credentials struct {
	CharacterID  string `json:"character_id"`
	UserID       string `json:"user_id"`
	GameID       string `json:"game_id"`
	CobaltCookie string `json:"cobalt_cookie"`
}

// SaveCredentials applies captured IDs to device.toml and the cookie to
// secrets.toml, then restarts the D&D Beyond side.
func (a *App) SaveCredentials(c Credentials) error {
	a.mu.Lock()
	d := a.cfg.Device
	if c.CharacterID != "" {
		d.CharacterID = c.CharacterID
	}
	if c.UserID != "" {
		d.UserID = c.UserID
	}
	if c.GameID != "" {
		d.GameID = c.GameID
	}
	s := a.secrets
	if c.CobaltCookie != "" {
		s.CobaltCookie = c.CobaltCookie
	}
	a.mu.Unlock()

	if err := config.SaveDevice(a.configDir, d); err != nil {
		return err
	}
	if c.CobaltCookie != "" {
		if err := config.SaveSecrets(a.configDir, s); err != nil {
			return err
		}
	}

	a.mu.Lock()
	a.cfg.Device = d
	a.secrets = s
	a.startDDBLocked()
	a.mu.Unlock()
	return nil
}
