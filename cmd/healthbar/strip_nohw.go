//go:build !hw

package main

import (
	"fmt"

	"github.com/will/dnd-health-tracker/internal/config"
	"github.com/will/dnd-health-tracker/internal/led"
)

// hardwareStrip is unavailable in non-hardware builds. Rebuild with "-tags hw"
// on the Raspberry Pi (with the rpi_ws281x C library installed) to drive a real
// strip, or run with --sim.
func hardwareStrip(config.Device) (led.Strip, error) {
	return nil, fmt.Errorf("hardware backend not built: rebuild with -tags hw, or run with --sim")
}
