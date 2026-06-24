// Package web serves the no-auth configuration UI for the health bar: a status
// page plus editors for the player, theme, WiFi, and D&D Beyond credentials. It
// talks to the runtime only through the Backend interface, so it stays pure
// presentation and is easy to test with a fake.
package web

import (
	"encoding/json"
	"html/template"
	"net/http"
	"strconv"
	"strings"

	"github.com/will/dnd-health-tracker/internal/app"
	"github.com/will/dnd-health-tracker/internal/config"
)

// Backend is the runtime surface the web UI needs (implemented by *app.App).
type Backend interface {
	Snapshot() app.Status
	Device() config.Device
	Theme() config.Theme
	WiFiNetworks() []config.WiFiNetwork
	SaveDevice(config.Device) error
	SaveTheme(config.Theme) error
	UpsertWiFi(config.WiFiNetwork) error
	RemoveWiFi(ssid string) error
	SaveCredentials(app.Credentials) error
}

// Server renders the config UI against a Backend.
type Server struct {
	backend Backend
}

// NewServer builds the web server.
func NewServer(b Backend) *Server { return &Server{backend: b} }

// Handler returns the HTTP handler with all routes registered.
func (s *Server) Handler() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("/", s.handleStatus)
	mux.HandleFunc("/device", s.handleDevice)
	mux.HandleFunc("/theme", s.handleTheme)
	mux.HandleFunc("/wifi", s.handleWiFi)
	mux.HandleFunc("/wifi/remove", s.handleWiFiRemove)
	mux.HandleFunc("/credentials", s.handleCredentials)
	mux.HandleFunc("/api/capture", s.handleCapture)
	return mux
}

func (s *Server) handleStatus(w http.ResponseWriter, r *http.Request) {
	if r.URL.Path != "/" {
		http.NotFound(w, r)
		return
	}
	render(w, "status", viewData{Title: "Status", Status: s.backend.Snapshot()})
}

func (s *Server) handleDevice(w http.ResponseWriter, r *http.Request) {
	if r.Method == http.MethodPost {
		d := s.backend.Device()
		d.PlayerName = strings.TrimSpace(r.FormValue("player_name"))
		d.CharacterID = strings.TrimSpace(r.FormValue("character_id"))
		d.UserID = strings.TrimSpace(r.FormValue("user_id"))
		d.GameID = strings.TrimSpace(r.FormValue("game_id"))
		d.NumLEDs = atoiOr(r.FormValue("num_leds"), d.NumLEDs)
		d.GPIOPin = atoiOr(r.FormValue("gpio_pin"), d.GPIOPin)
		d.Brightness = atofOr(r.FormValue("brightness"), d.Brightness)
		d.PollFastSeconds = atofOr(r.FormValue("poll_fast_seconds"), d.PollFastSeconds)
		d.PollIdleSeconds = atofOr(r.FormValue("poll_idle_seconds"), d.PollIdleSeconds)
		if err := s.backend.SaveDevice(d); err != nil {
			http.Error(w, err.Error(), http.StatusInternalServerError)
			return
		}
		http.Redirect(w, r, "/device?ok=1", http.StatusSeeOther)
		return
	}
	render(w, "device", viewData{Title: "Player", Device: s.backend.Device(), Flash: flash(r)})
}

func (s *Server) handleTheme(w http.ResponseWriter, r *http.Request) {
	if r.Method == http.MethodPost {
		t := s.backend.Theme()
		setHex(&t.HPHigh, r.FormValue("hp_high"))
		setHex(&t.HPMid, r.FormValue("hp_mid"))
		setHex(&t.HPLow, r.FormValue("hp_low"))
		setHex(&t.Temp, r.FormValue("temp_hp"))
		setHex(&t.Status, r.FormValue("status"))
		t.MidFraction = atofOr(r.FormValue("mid_fraction"), t.MidFraction)
		t.LowFraction = atofOr(r.FormValue("low_fraction"), t.LowFraction)
		t.Brightness = atofOr(r.FormValue("brightness"), t.Brightness)
		t.FPS = atoiOr(r.FormValue("fps"), t.FPS)
		t.FlashMillis = atoiOr(r.FormValue("flash_millis"), t.FlashMillis)
		t.AdjustMillis = atoiOr(r.FormValue("adjust_millis"), t.AdjustMillis)
		t.IdleShimmer = atofOr(r.FormValue("idle_shimmer"), t.IdleShimmer)
		t.IdleShimmerHz = atofOr(r.FormValue("idle_shimmer_hz"), t.IdleShimmerHz)
		t.LowPulseHz = atofOr(r.FormValue("low_pulse_hz"), t.LowPulseHz)
		if err := s.backend.SaveTheme(t); err != nil {
			http.Error(w, err.Error(), http.StatusInternalServerError)
			return
		}
		http.Redirect(w, r, "/theme?ok=1", http.StatusSeeOther)
		return
	}
	render(w, "theme", viewData{Title: "Theme", Theme: s.backend.Theme(), Flash: flash(r)})
}

func (s *Server) handleWiFi(w http.ResponseWriter, r *http.Request) {
	if r.Method == http.MethodPost {
		ssid := strings.TrimSpace(r.FormValue("ssid"))
		if ssid == "" {
			http.Error(w, "ssid required", http.StatusBadRequest)
			return
		}
		n := config.WiFiNetwork{SSID: ssid, PSK: r.FormValue("psk"), Priority: atoiOr(r.FormValue("priority"), 0)}
		if err := s.backend.UpsertWiFi(n); err != nil {
			http.Error(w, err.Error(), http.StatusInternalServerError)
			return
		}
		http.Redirect(w, r, "/wifi?ok=1", http.StatusSeeOther)
		return
	}
	render(w, "wifi", viewData{Title: "WiFi", Networks: s.backend.WiFiNetworks(), Flash: flash(r)})
}

func (s *Server) handleWiFiRemove(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return
	}
	if err := s.backend.RemoveWiFi(r.FormValue("ssid")); err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	http.Redirect(w, r, "/wifi?ok=1", http.StatusSeeOther)
}

func (s *Server) handleCredentials(w http.ResponseWriter, r *http.Request) {
	if r.Method == http.MethodPost {
		c := app.Credentials{
			CharacterID:  strings.TrimSpace(r.FormValue("character_id")),
			UserID:       strings.TrimSpace(r.FormValue("user_id")),
			GameID:       strings.TrimSpace(r.FormValue("game_id")),
			CobaltCookie: strings.TrimSpace(r.FormValue("cobalt_cookie")),
		}
		if err := s.backend.SaveCredentials(c); err != nil {
			http.Error(w, err.Error(), http.StatusInternalServerError)
			return
		}
		http.Redirect(w, r, "/?ok=1", http.StatusSeeOther)
		return
	}
	render(w, "credentials", viewData{
		Title:       "D&D Beyond",
		Device:      s.backend.Device(),
		Status:      s.backend.Snapshot(),
		Bookmarklet: bookmarklet(r.Host),
		Flash:       flash(r),
	})
}

// handleCapture receives JSON from the bookmarklet (cross-origin), so it sets
// permissive CORS headers and answers the preflight.
func (s *Server) handleCapture(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Access-Control-Allow-Origin", "*")
	w.Header().Set("Access-Control-Allow-Headers", "Content-Type")
	if r.Method == http.MethodOptions {
		w.WriteHeader(http.StatusNoContent)
		return
	}
	if r.Method != http.MethodPost {
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return
	}
	var c app.Credentials
	if err := json.NewDecoder(http.MaxBytesReader(w, r.Body, 1<<16)).Decode(&c); err != nil {
		http.Error(w, "bad json", http.StatusBadRequest)
		return
	}
	if err := s.backend.SaveCredentials(c); err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	w.Header().Set("Content-Type", "application/json")
	w.Write([]byte(`{"ok":true}`))
}

// bookmarklet builds a javascript: bookmarklet that reads the character id from
// the current D&D Beyond URL and POSTs it back to this bar.
func bookmarklet(host string) template.URL {
	base := "http://" + host
	js := `javascript:(function(){` +
		`var m=location.pathname.match(/characters?\/(\d+)/);` +
		`var c=m?m[1]:prompt('D&D Beyond character id?');if(!c)return;` +
		`fetch('` + base + `/api/capture',{method:'POST',headers:{'Content-Type':'application/json'},` +
		`body:JSON.stringify({character_id:c})})` +
		`.then(function(){alert('Sent character '+c+' to the health bar!')})` +
		`.catch(function(e){alert('Failed: '+e)})})()`
	return template.URL(js)
}

func flash(r *http.Request) string {
	if r.URL.Query().Get("ok") == "1" {
		return "Saved."
	}
	return ""
}

func atoiOr(s string, def int) int {
	if v, err := strconv.Atoi(strings.TrimSpace(s)); err == nil {
		return v
	}
	return def
}

func atofOr(s string, def float64) float64 {
	if v, err := strconv.ParseFloat(strings.TrimSpace(s), 64); err == nil {
		return v
	}
	return def
}

// setHex updates a color from a form value, leaving it unchanged if blank/invalid.
func setHex(h *config.HexColor, s string) {
	if strings.TrimSpace(s) == "" {
		return
	}
	_ = h.UnmarshalText([]byte(s))
}
