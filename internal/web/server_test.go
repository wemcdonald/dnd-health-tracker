package web

import (
	"net/http"
	"net/http/httptest"
	"net/url"
	"strings"
	"testing"

	"github.com/will/dnd-health-tracker/internal/app"
	"github.com/will/dnd-health-tracker/internal/config"
)

type fakeBackend struct {
	device   config.Device
	theme    config.Theme
	wifi     []config.WiFiNetwork
	status   app.Status
	savedDev *config.Device
	savedThm *config.Theme
	upserted *config.WiFiNetwork
	removed  string
	creds    *app.Credentials
}

func (f *fakeBackend) Snapshot() app.Status                  { return f.status }
func (f *fakeBackend) Device() config.Device                 { return f.device }
func (f *fakeBackend) Theme() config.Theme                   { return f.theme }
func (f *fakeBackend) WiFiNetworks() []config.WiFiNetwork    { return f.wifi }
func (f *fakeBackend) SaveDevice(d config.Device) error      { f.savedDev = &d; return nil }
func (f *fakeBackend) SaveTheme(t config.Theme) error        { f.savedThm = &t; return nil }
func (f *fakeBackend) UpsertWiFi(n config.WiFiNetwork) error { f.upserted = &n; return nil }
func (f *fakeBackend) RemoveWiFi(ssid string) error          { f.removed = ssid; return nil }
func (f *fakeBackend) SaveCredentials(c app.Credentials) error {
	f.creds = &c
	return nil
}

func newTestServer() (*fakeBackend, http.Handler) {
	b := &fakeBackend{
		device: config.DefaultDevice(),
		theme:  config.DefaultTheme(),
		status: app.Status{Player: "Aldric", CharacterID: "42", Connection: "online", HP: 30, MaxHP: 45},
	}
	b.device.PlayerName = "Aldric"
	return b, NewServer(b).Handler()
}

func do(h http.Handler, method, target string, form url.Values) *httptest.ResponseRecorder {
	var req *http.Request
	if form != nil {
		req = httptest.NewRequest(method, target, strings.NewReader(form.Encode()))
		req.Header.Set("Content-Type", "application/x-www-form-urlencoded")
	} else {
		req = httptest.NewRequest(method, target, nil)
	}
	rr := httptest.NewRecorder()
	h.ServeHTTP(rr, req)
	return rr
}

func TestStatusPageRenders(t *testing.T) {
	_, h := newTestServer()
	rr := do(h, http.MethodGet, "/", nil)
	if rr.Code != 200 {
		t.Fatalf("status %d", rr.Code)
	}
	body := rr.Body.String()
	if !strings.Contains(body, "Aldric") || !strings.Contains(body, "30 / 45") {
		t.Errorf("status page missing player/HP: %s", body)
	}
}

func TestSaveDevice(t *testing.T) {
	b, h := newTestServer()
	form := url.Values{"player_name": {"Borin"}, "character_id": {"999"}, "num_leds": {"17"}, "brightness": {"0.7"}}
	rr := do(h, http.MethodPost, "/device", form)
	if rr.Code != http.StatusSeeOther {
		t.Fatalf("expected redirect, got %d", rr.Code)
	}
	if b.savedDev == nil || b.savedDev.PlayerName != "Borin" || b.savedDev.NumLEDs != 17 {
		t.Errorf("device not saved correctly: %+v", b.savedDev)
	}
	if b.savedDev.Brightness != 0.7 {
		t.Errorf("brightness = %v", b.savedDev.Brightness)
	}
}

func TestSaveThemeParsesColors(t *testing.T) {
	b, h := newTestServer()
	form := url.Values{"hp_high": {"#abcdef"}, "fps": {"30"}}
	if rr := do(h, http.MethodPost, "/theme", form); rr.Code != http.StatusSeeOther {
		t.Fatalf("expected redirect, got %d", rr.Code)
	}
	if b.savedThm == nil || b.savedThm.FPS != 30 {
		t.Fatalf("theme not saved: %+v", b.savedThm)
	}
	if got, _ := b.savedThm.HPHigh.MarshalText(); string(got) != "#abcdef" {
		t.Errorf("hp_high = %s", got)
	}
}

func TestWiFiUpsertAndRemove(t *testing.T) {
	b, h := newTestServer()
	do(h, http.MethodPost, "/wifi", url.Values{"ssid": {"Home"}, "psk": {"pw"}, "priority": {"5"}})
	if b.upserted == nil || b.upserted.SSID != "Home" || b.upserted.Priority != 5 {
		t.Errorf("wifi upsert wrong: %+v", b.upserted)
	}
	do(h, http.MethodPost, "/wifi/remove", url.Values{"ssid": {"Home"}})
	if b.removed != "Home" {
		t.Errorf("wifi remove = %q", b.removed)
	}
}

func TestCredentialsPageHasBookmarklet(t *testing.T) {
	_, h := newTestServer()
	body := do(h, http.MethodGet, "/credentials", nil).Body.String()
	if !strings.Contains(body, "Send to Health Bar") || !strings.Contains(body, "javascript:") {
		t.Errorf("credentials page missing bookmarklet")
	}
	if !strings.Contains(body, "/api/capture") {
		t.Errorf("bookmarklet should target /api/capture")
	}
}

func TestCaptureJSONAndCORS(t *testing.T) {
	b, h := newTestServer()
	req := httptest.NewRequest(http.MethodPost, "/api/capture", strings.NewReader(`{"character_id":"777"}`))
	req.Header.Set("Content-Type", "application/json")
	rr := httptest.NewRecorder()
	h.ServeHTTP(rr, req)
	if rr.Code != 200 {
		t.Fatalf("capture status %d", rr.Code)
	}
	if rr.Header().Get("Access-Control-Allow-Origin") != "*" {
		t.Error("missing CORS header")
	}
	if b.creds == nil || b.creds.CharacterID != "777" {
		t.Errorf("credentials not captured: %+v", b.creds)
	}

	// Preflight.
	if rr := do(h, http.MethodOptions, "/api/capture", nil); rr.Code != http.StatusNoContent {
		t.Errorf("OPTIONS preflight = %d", rr.Code)
	}
}
