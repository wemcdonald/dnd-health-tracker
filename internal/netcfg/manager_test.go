package netcfg

import (
	"context"
	"strings"
	"testing"

	"github.com/will/dnd-health-tracker/internal/config"
)

type fakeRunner struct {
	calls   [][]string
	outputs map[string]string // keyed by space-joined args
}

func (f *fakeRunner) Run(_ context.Context, name string, args ...string) (string, error) {
	f.calls = append(f.calls, append([]string{name}, args...))
	if out, ok := f.outputs[strings.Join(args, " ")]; ok {
		return out, nil
	}
	return "", nil
}

// hasCall reports whether some recorded call contains all the given substrings.
func (f *fakeRunner) hasCall(subs ...string) bool {
	for _, call := range f.calls {
		joined := strings.Join(call, " ")
		all := true
		for _, s := range subs {
			if !strings.Contains(joined, s) {
				all = false
				break
			}
		}
		if all {
			return true
		}
	}
	return false
}

func TestSyncCreatesProfilesAndPrunesStale(t *testing.T) {
	fr := &fakeRunner{outputs: map[string]string{
		"-t -f NAME connection show": "healthbar-OldNet\nWiredConn\n",
	}}
	m := &Manager{Runner: fr}
	w := config.WiFi{Networks: []config.WiFiNetwork{
		{SSID: "HomeNet", PSK: "secret123", Priority: 10},
		{SSID: "OpenCafe", Priority: 1},
	}}
	if err := m.Sync(context.Background(), w); err != nil {
		t.Fatal(err)
	}

	if !fr.hasCall("connection", "add", "ssid", "HomeNet", "wifi-sec.psk", "secret123", "autoconnect-priority", "10") {
		t.Error("expected add for HomeNet with psk and priority")
	}
	if !fr.hasCall("connection", "add", "ssid", "OpenCafe") {
		t.Error("expected add for open network OpenCafe")
	}
	// Open network must not get a psk.
	for _, c := range fr.calls {
		j := strings.Join(c, " ")
		if strings.Contains(j, "OpenCafe") && strings.Contains(j, "wifi-sec.psk") {
			t.Error("open network should not have a psk")
		}
	}
	if !fr.hasCall("connection", "delete", "healthbar-OldNet") {
		t.Error("expected stale healthbar-OldNet to be deleted")
	}
	// Must not touch the user's own non-prefixed connection.
	if fr.hasCall("connection", "delete", "WiredConn") {
		t.Error("must not delete connections we don't own")
	}
}

func TestConnectivity(t *testing.T) {
	full := &Manager{Runner: &fakeRunner{outputs: map[string]string{"-t networking connectivity": "full\n"}}}
	if !full.Connectivity(context.Background()) {
		t.Error("expected full connectivity to be true")
	}
	none := &Manager{Runner: &fakeRunner{outputs: map[string]string{"-t networking connectivity": "none\n"}}}
	if none.Connectivity(context.Background()) {
		t.Error("expected none connectivity to be false")
	}
}

func TestHotspotLifecycle(t *testing.T) {
	fr := &fakeRunner{}
	m := &Manager{Runner: fr, HotspotSSID: "healthbar-setup", HotspotPassword: "pw"}
	if err := m.StartHotspot(context.Background()); err != nil {
		t.Fatal(err)
	}
	if !m.HotspotActive() {
		t.Error("hotspot should be active after start")
	}
	if !fr.hasCall("device", "wifi", "hotspot", "ssid", "healthbar-setup", "password", "pw") {
		t.Error("expected hotspot command")
	}
	if err := m.StopHotspot(context.Background()); err != nil {
		t.Fatal(err)
	}
	if m.HotspotActive() {
		t.Error("hotspot should be inactive after stop")
	}
}
