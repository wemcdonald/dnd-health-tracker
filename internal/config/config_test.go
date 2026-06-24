package config

import (
	"os"
	"path/filepath"
	"testing"

	"github.com/will/dnd-health-tracker/internal/led"
)

func TestLoadMissingUsesDefaults(t *testing.T) {
	cfg, err := Load(t.TempDir()) // empty dir => all defaults
	if err != nil {
		t.Fatal(err)
	}
	if cfg.Device.NumLEDs != 16 || cfg.Theme.FPS != 60 {
		t.Errorf("defaults not applied: %+v / %+v", cfg.Device, cfg.Theme)
	}
}

func TestLoadFromFiles(t *testing.T) {
	dir := t.TempDir()
	device := `player_name = "Aldric"
character_id = "12345"
num_leds = 17
brightness = 0.8
`
	theme := `hp_high = "#00ff00"
fps = 30
`
	mustWrite(t, filepath.Join(dir, "device.toml"), device)
	mustWrite(t, filepath.Join(dir, "theme.toml"), theme)

	cfg, err := Load(dir)
	if err != nil {
		t.Fatal(err)
	}
	if cfg.Device.PlayerName != "Aldric" || cfg.Device.NumLEDs != 17 {
		t.Errorf("device not loaded: %+v", cfg.Device)
	}
	if cfg.Theme.FPS != 30 {
		t.Errorf("theme fps not loaded: %d", cfg.Theme.FPS)
	}
	if cfg.Theme.HPHigh.Color() != led.RGB(0, 255, 0) {
		t.Errorf("hp_high color = %v", cfg.Theme.HPHigh.Color())
	}
	// Field absent from theme.toml keeps its default.
	if cfg.Theme.LowFraction != DefaultTheme().LowFraction {
		t.Errorf("absent field should keep default, got %v", cfg.Theme.LowFraction)
	}
}

func TestLoadInvalidIsError(t *testing.T) {
	dir := t.TempDir()
	mustWrite(t, filepath.Join(dir, "device.toml"), "num_leds = \"not a number\"\n")
	if _, err := Load(dir); err == nil {
		t.Error("expected error for invalid TOML value")
	}
}

func TestHexColorRoundTrip(t *testing.T) {
	var h HexColor
	if err := h.UnmarshalText([]byte("#1a2b3c")); err != nil {
		t.Fatal(err)
	}
	if h.Color() != led.RGB(0x1a, 0x2b, 0x3c) {
		t.Errorf("parsed wrong: %v", h.Color())
	}
	out, err := h.MarshalText()
	if err != nil {
		t.Fatal(err)
	}
	if string(out) != "#1a2b3c" {
		t.Errorf("marshal = %q", out)
	}
	if err := h.UnmarshalText([]byte("xyz")); err == nil {
		t.Error("expected error for bad hex")
	}
}

func mustWrite(t *testing.T, path, content string) {
	t.Helper()
	if err := os.WriteFile(path, []byte(content), 0o644); err != nil {
		t.Fatal(err)
	}
}
