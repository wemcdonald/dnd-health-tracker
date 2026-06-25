//go:build hw

package main

import (
	"github.com/will/dnd-health-tracker/internal/config"
	"github.com/will/dnd-health-tracker/internal/led"
)

// hardwareStrip opens the real WS2812B strip. Built only with "-tags hw".
func hardwareStrip(d config.Device) (led.Strip, error) {
	return led.NewHardware(led.HardwareOptions{
		GPIOPin:    d.GPIOPin,
		NumLEDs:    d.NumLEDs,
		Brightness: d.Brightness,
	})
}
