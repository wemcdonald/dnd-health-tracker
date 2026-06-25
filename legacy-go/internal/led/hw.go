//go:build hw

// Package led hardware backend. Built only with the "hw" build tag (i.e. on
// the Raspberry Pi, where the jgarff rpi_ws281x C library is installed):
//
//	go build -tags hw ./cmd/healthbar
//
// Without the tag this file is excluded, so laptop and CI builds need neither
// cgo nor the C library.
package led

import (
	"fmt"

	ws2811 "github.com/rpi-ws281x/rpi-ws281x-go"
)

// Hardware drives a real WS2812B strip over DMA/PWM via GPIO. Requires root.
type Hardware struct {
	dev *ws2811.WS2811
	n   int
}

// HardwareOptions configures the physical strip.
type HardwareOptions struct {
	GPIOPin    int     // data pin (BCM numbering); GPIO18 is the usual PWM pin
	NumLEDs    int     // number of pixels on the strip
	Brightness float64 // global hardware brightness 0..1
}

// NewHardware initialises the strip. Call Close to release the DMA channel.
func NewHardware(o HardwareOptions) (*Hardware, error) {
	opt := ws2811.DefaultOptions
	opt.Channels[0].GpioPin = o.GPIOPin
	opt.Channels[0].LedCount = o.NumLEDs
	opt.Channels[0].Brightness = int(clamp01(o.Brightness) * 255)

	dev, err := ws2811.MakeWS2811(&opt)
	if err != nil {
		return nil, fmt.Errorf("led: make ws2811: %w", err)
	}
	if err := dev.Init(); err != nil {
		return nil, fmt.Errorf("led: init ws2811 (root required): %w", err)
	}
	return &Hardware{dev: dev, n: o.NumLEDs}, nil
}

// Len reports the pixel count.
func (h *Hardware) Len() int { return h.n }

// Render pushes the frame to the strip. The C library applies the configured
// strip color order (WS2812 = GRB), so we pack plain 0x00RRGGBB here.
func (h *Hardware) Render(frame Frame) error {
	if len(frame) != h.n {
		return fmt.Errorf("led: frame length %d != strip length %d", len(frame), h.n)
	}
	leds := h.dev.Leds(0)
	for i, c := range frame {
		leds[i] = uint32(c.R)<<16 | uint32(c.G)<<8 | uint32(c.B)
	}
	return h.dev.Render()
}

// Close blanks the strip and releases hardware resources.
func (h *Hardware) Close() error {
	if h.dev != nil {
		h.Render(NewFrame(h.n)) // best-effort blank
		h.dev.Fini()
	}
	return nil
}
