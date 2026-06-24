// Package config loads the health bar's TOML configuration. Files are simple
// enough to hand-edit on the SD card and are also written by the web UI.
//
// Two files live in the config directory:
//   - device.toml: per-unit identity + hardware (player, character, LEDs, pins)
//   - theme.toml:  all tunable look-and-feel constants (colors, timing, ...)
//
// Loading is tolerant: a missing file yields built-in defaults so a fresh unit
// boots into a sane demo/idle state before it is configured.
package config

import (
	"fmt"
	"os"
	"path/filepath"
	"time"

	"github.com/BurntSushi/toml"
	"github.com/will/dnd-health-tracker/internal/led"
)

// Device is per-unit identity and hardware configuration (device.toml).
type Device struct {
	PlayerName  string `toml:"player_name"`
	CharacterID string `toml:"character_id"`
	UserID      string `toml:"user_id"` // optional, needed for websocket
	GameID      string `toml:"game_id"` // optional, enables websocket push

	NumLEDs    int     `toml:"num_leds"`
	GPIOPin    int     `toml:"gpio_pin"`
	Brightness float64 `toml:"brightness"` // hardware brightness 0..1

	PollFastSeconds float64 `toml:"poll_fast_seconds"` // poll interval right after a change
	PollIdleSeconds float64 `toml:"poll_idle_seconds"` // poll interval when idle
}

// PollFast returns the active polling interval.
func (d Device) PollFast() time.Duration {
	return time.Duration(d.PollFastSeconds * float64(time.Second))
}

// PollIdle returns the idle polling interval.
func (d Device) PollIdle() time.Duration {
	return time.Duration(d.PollIdleSeconds * float64(time.Second))
}

// Theme holds every look-and-feel constant. Read at startup and re-read on
// save so colors/timing can be tweaked without code changes.
type Theme struct {
	// Colors
	HPHigh HexColor `toml:"hp_high"` // full health
	HPMid  HexColor `toml:"hp_mid"`  // mid health
	HPLow  HexColor `toml:"hp_low"`  // near death
	Temp   HexColor `toml:"temp_hp"` // temporary HP overshield
	Flash  HexColor `toml:"flash"`   // generic flash overlay (unused base)
	Status HexColor `toml:"status"`  // status/connecting color

	// Thresholds (fraction of max HP)
	MidFraction float64 `toml:"mid_fraction"`
	LowFraction float64 `toml:"low_fraction"`

	// Render
	Brightness float64 `toml:"brightness"` // render-time multiplier 0..1
	FPS        int     `toml:"fps"`

	// Animation tuning (used from Phase 2 onward)
	FlashMillis   int     `toml:"flash_millis"`    // damage/heal flash duration
	AdjustMillis  int     `toml:"adjust_millis"`   // bar grow/shrink duration
	IdleShimmer   float64 `toml:"idle_shimmer"`    // breathing amplitude 0..1
	IdleShimmerHz float64 `toml:"idle_shimmer_hz"` // breathing speed
	LowPulseHz    float64 `toml:"low_pulse_hz"`    // low-HP heartbeat speed
}

// Config is the aggregate of all loaded files.
type Config struct {
	Device Device
	Theme  Theme
}

// DefaultDevice returns built-in device defaults (16-LED strip on GPIO18).
func DefaultDevice() Device {
	return Device{
		PlayerName:      "",
		NumLEDs:         16,
		GPIOPin:         18,
		Brightness:      0.5,
		PollFastSeconds: 3,
		PollIdleSeconds: 25,
	}
}

// DefaultTheme returns built-in theme defaults.
func DefaultTheme() Theme {
	return Theme{
		HPHigh:        HexColor(led.RGB(0, 220, 60)),
		HPMid:         HexColor(led.RGB(230, 180, 0)),
		HPLow:         HexColor(led.RGB(220, 20, 20)),
		Temp:          HexColor(led.RGB(60, 140, 255)),
		Flash:         HexColor(led.RGB(255, 255, 255)),
		Status:        HexColor(led.RGB(80, 80, 160)),
		MidFraction:   0.5,
		LowFraction:   0.25,
		Brightness:    1.0,
		FPS:           60,
		FlashMillis:   220,
		AdjustMillis:  600,
		IdleShimmer:   0.08,
		IdleShimmerHz: 0.25,
		LowPulseHz:    1.4,
	}
}

// Default returns a fully-defaulted configuration.
func Default() Config {
	return Config{Device: DefaultDevice(), Theme: DefaultTheme()}
}

// Load reads device.toml and theme.toml from dir, falling back to defaults for
// any file that is absent. Present-but-invalid files are a hard error.
func Load(dir string) (Config, error) {
	cfg := Default()
	if err := loadFile(filepath.Join(dir, "device.toml"), &cfg.Device); err != nil {
		return cfg, err
	}
	if err := loadFile(filepath.Join(dir, "theme.toml"), &cfg.Theme); err != nil {
		return cfg, err
	}
	cfg.applyFloors()
	return cfg, nil
}

// loadFile decodes a TOML file into v. A missing file is not an error (defaults
// are kept); decode failures are reported.
func loadFile(path string, v any) error {
	if _, err := os.Stat(path); os.IsNotExist(err) {
		return nil
	}
	if _, err := toml.DecodeFile(path, v); err != nil {
		return fmt.Errorf("config: decode %s: %w", path, err)
	}
	return nil
}

// applyFloors guards against zero/invalid values that would break rendering.
func (c *Config) applyFloors() {
	if c.Device.NumLEDs <= 0 {
		c.Device.NumLEDs = DefaultDevice().NumLEDs
	}
	if c.Theme.FPS <= 0 {
		c.Theme.FPS = DefaultTheme().FPS
	}
	if c.Device.PollFastSeconds <= 0 {
		c.Device.PollFastSeconds = DefaultDevice().PollFastSeconds
	}
	if c.Device.PollIdleSeconds <= 0 {
		c.Device.PollIdleSeconds = DefaultDevice().PollIdleSeconds
	}
}
