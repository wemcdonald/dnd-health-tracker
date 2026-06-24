package config

import (
	"fmt"
	"os"
	"path/filepath"
	"sort"

	"github.com/BurntSushi/toml"
)

const wifiFile = "wifi.toml"

// WiFiNetwork is one known network. Higher Priority wins when several known
// networks are in range (mapped to NetworkManager autoconnect priority).
type WiFiNetwork struct {
	SSID     string `toml:"ssid"`
	PSK      string `toml:"psk"`
	Priority int    `toml:"priority"`
}

// WiFi is the source-of-truth list of known networks, synced to the OS network
// manager at boot and whenever edited via the web UI.
type WiFi struct {
	Networks []WiFiNetwork `toml:"networks"`
}

// LoadWiFi reads wifi.toml from dir; a missing file yields an empty list.
func LoadWiFi(dir string) (WiFi, error) {
	var w WiFi
	path := filepath.Join(dir, wifiFile)
	if _, err := os.Stat(path); os.IsNotExist(err) {
		return w, nil
	}
	if _, err := toml.DecodeFile(path, &w); err != nil {
		return w, fmt.Errorf("config: decode %s: %w", path, err)
	}
	return w, nil
}

// SaveWiFi writes wifi.toml.
func SaveWiFi(dir string, w WiFi) error {
	path := filepath.Join(dir, wifiFile)
	f, err := os.OpenFile(path, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, 0o644)
	if err != nil {
		return fmt.Errorf("config: open %s: %w", path, err)
	}
	defer f.Close()
	if err := toml.NewEncoder(f).Encode(w); err != nil {
		return fmt.Errorf("config: encode wifi: %w", err)
	}
	return nil
}

// Upsert adds or updates a network by SSID, keeping the list sorted by
// descending priority then SSID for stable display.
func (w *WiFi) Upsert(n WiFiNetwork) {
	found := false
	for i := range w.Networks {
		if w.Networks[i].SSID == n.SSID {
			w.Networks[i] = n
			found = true
			break
		}
	}
	if !found {
		w.Networks = append(w.Networks, n)
	}
	sort.SliceStable(w.Networks, func(i, j int) bool {
		if w.Networks[i].Priority != w.Networks[j].Priority {
			return w.Networks[i].Priority > w.Networks[j].Priority
		}
		return w.Networks[i].SSID < w.Networks[j].SSID
	})
}

// Remove deletes a network by SSID, reporting whether it existed.
func (w *WiFi) Remove(ssid string) bool {
	for i := range w.Networks {
		if w.Networks[i].SSID == ssid {
			w.Networks = append(w.Networks[:i], w.Networks[i+1:]...)
			return true
		}
	}
	return false
}
