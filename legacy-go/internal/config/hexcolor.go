package config

import (
	"fmt"
	"strings"

	"github.com/will/dnd-health-tracker/internal/led"
)

// HexColor is a led.Color that marshals to/from a "#RRGGBB" TOML string so
// colors are human-editable in the config files.
type HexColor led.Color

// Color returns the underlying led.Color.
func (h HexColor) Color() led.Color { return led.Color(h) }

// UnmarshalText parses "#RRGGBB" (the leading '#' is optional).
func (h *HexColor) UnmarshalText(text []byte) error {
	s := strings.TrimPrefix(strings.TrimSpace(string(text)), "#")
	if len(s) != 6 {
		return fmt.Errorf("config: invalid hex color %q (want #RRGGBB)", string(text))
	}
	var r, g, b uint8
	if _, err := fmt.Sscanf(s, "%02x%02x%02x", &r, &g, &b); err != nil {
		return fmt.Errorf("config: invalid hex color %q: %w", string(text), err)
	}
	*h = HexColor(led.RGB(r, g, b))
	return nil
}

// MarshalText renders the color as "#RRGGBB" for writing config back out.
func (h HexColor) MarshalText() ([]byte, error) {
	return []byte(fmt.Sprintf("#%02x%02x%02x", h.R, h.G, h.B)), nil
}
