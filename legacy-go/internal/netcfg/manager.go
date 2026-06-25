package netcfg

import (
	"context"
	"sort"
	"strconv"
	"strings"

	"github.com/will/dnd-health-tracker/internal/config"
)

// connPrefix namespaces the NetworkManager connection profiles this app owns,
// so syncing never touches connections the user created by hand.
const connPrefix = "healthbar-"

// Manager drives NetworkManager via nmcli.
type Manager struct {
	Runner          Runner
	HotspotSSID     string
	HotspotPassword string

	hotspotActive bool
}

// NewManager returns a Manager using the real nmcli runner.
func NewManager() *Manager {
	return &Manager{
		Runner:          ExecRunner{},
		HotspotSSID:     "healthbar-setup",
		HotspotPassword: "dndhealthbar",
	}
}

// Sync reconciles NetworkManager profiles with the desired known-networks list:
// it deletes our stale profiles and (re)creates one per network with the right
// autoconnect priority, so the Pi joins the best known network automatically.
func (m *Manager) Sync(ctx context.Context, w config.WiFi) error {
	desired := make(map[string]config.WiFiNetwork, len(w.Networks))
	for _, n := range w.Networks {
		if n.SSID != "" {
			desired[connName(n.SSID)] = n
		}
	}

	// Remove our profiles that are no longer wanted.
	for _, name := range m.listOurConnections(ctx) {
		if _, ok := desired[name]; !ok {
			_, _ = m.Runner.Run(ctx, "nmcli", "connection", "delete", name)
		}
	}

	var firstErr error
	for _, name := range sortedKeys(desired) {
		n := desired[name]
		// Delete-then-add keeps the operation idempotent across runs.
		_, _ = m.Runner.Run(ctx, "nmcli", "connection", "delete", name)
		args := []string{
			"connection", "add", "type", "wifi", "con-name", name, "ssid", n.SSID,
			"connection.autoconnect", "yes",
			"connection.autoconnect-priority", strconv.Itoa(n.Priority),
		}
		if n.PSK != "" {
			args = append(args, "wifi-sec.key-mgmt", "wpa-psk", "wifi-sec.psk", n.PSK)
		}
		if _, err := m.Runner.Run(ctx, "nmcli", args...); err != nil && firstErr == nil {
			firstErr = err
		}
	}
	return firstErr
}

// Connectivity reports whether NetworkManager has full internet connectivity.
func (m *Manager) Connectivity(ctx context.Context) bool {
	out, err := m.Runner.Run(ctx, "nmcli", "-t", "networking", "connectivity")
	if err != nil {
		return false
	}
	return strings.TrimSpace(out) == "full"
}

// StartHotspot brings up an open-config access point so a phone/laptop can
// reach the captive-portal config page.
func (m *Manager) StartHotspot(ctx context.Context) error {
	_, err := m.Runner.Run(ctx, "nmcli", "device", "wifi", "hotspot",
		"ssid", m.HotspotSSID, "password", m.HotspotPassword)
	if err == nil {
		m.hotspotActive = true
	}
	return err
}

// StopHotspot tears the access point down so the radio can rejoin known nets.
func (m *Manager) StopHotspot(ctx context.Context) error {
	_, err := m.Runner.Run(ctx, "nmcli", "connection", "down", "Hotspot")
	m.hotspotActive = false
	return err
}

// HotspotActive reports whether the fallback AP is currently up.
func (m *Manager) HotspotActive() bool { return m.hotspotActive }

// listOurConnections returns the names of NetworkManager profiles this app owns.
func (m *Manager) listOurConnections(ctx context.Context) []string {
	out, err := m.Runner.Run(ctx, "nmcli", "-t", "-f", "NAME", "connection", "show")
	if err != nil {
		return nil
	}
	var names []string
	for _, line := range strings.Split(out, "\n") {
		name := strings.TrimSpace(line)
		if strings.HasPrefix(name, connPrefix) {
			names = append(names, name)
		}
	}
	return names
}

func connName(ssid string) string { return connPrefix + ssid }

func sortedKeys(m map[string]config.WiFiNetwork) []string {
	keys := make([]string, 0, len(m))
	for k := range m {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	return keys
}
