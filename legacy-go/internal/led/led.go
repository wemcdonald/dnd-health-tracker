// Package led defines the LED strip abstraction used by the health bar.
//
// The Strip interface decouples animation/render code from the physical
// hardware so the entire application can run on a developer laptop using the
// terminal Simulator backend (no Raspberry Pi or WS2812B strip required).
// The real hardware backend lives in hw.go behind the "hw" build tag.
package led

import "math"

// Color is a 24-bit RGB color. WS2812B LEDs are GRB on the wire, but that
// ordering is handled by the hardware backend; callers always work in RGB.
type Color struct {
	R, G, B uint8
}

// Black is the off state for a pixel.
var Black = Color{0, 0, 0}

// RGB is a convenience constructor.
func RGB(r, g, b uint8) Color { return Color{r, g, b} }

// Scale multiplies each channel by f (clamped to [0,1]) for brightness control.
func (c Color) Scale(f float64) Color {
	f = clamp01(f)
	return Color{
		R: uint8(float64(c.R) * f),
		G: uint8(float64(c.G) * f),
		B: uint8(float64(c.B) * f),
	}
}

// Lerp linearly interpolates between a and b. t is clamped to [0,1].
func Lerp(a, b Color, t float64) Color {
	t = clamp01(t)
	return Color{
		R: lerpU8(a.R, b.R, t),
		G: lerpU8(a.G, b.G, t),
		B: lerpU8(a.B, b.B, t),
	}
}

// Add returns the saturating sum of two colors (useful for overlaying effects).
func (c Color) Add(o Color) Color {
	return Color{
		R: addU8(c.R, o.R),
		G: addU8(c.G, o.G),
		B: addU8(c.B, o.B),
	}
}

// Frame is a full strip's worth of pixels, index 0 being the first LED.
type Frame []Color

// NewFrame allocates an all-black frame of length n.
func NewFrame(n int) Frame { return make(Frame, n) }

// Fill sets every pixel to c.
func (f Frame) Fill(c Color) {
	for i := range f {
		f[i] = c
	}
}

// Clear turns every pixel off.
func (f Frame) Clear() { f.Fill(Black) }

// Clone returns an independent copy of the frame.
func (f Frame) Clone() Frame {
	c := make(Frame, len(f))
	copy(c, f)
	return c
}

// Strip is the minimal contract a physical or simulated LED strip must fulfil.
// Implementations must be safe to call Render on repeatedly from a single
// render goroutine.
type Strip interface {
	// Len reports the number of pixels.
	Len() int
	// Render pushes a frame to the strip. len(frame) must equal Len().
	Render(frame Frame) error
	// Close releases any resources (DMA channels, terminal state, ...).
	Close() error
}

func clamp01(f float64) float64 {
	if f < 0 {
		return 0
	}
	if f > 1 {
		return 1
	}
	if math.IsNaN(f) {
		return 0
	}
	return f
}

func lerpU8(a, b uint8, t float64) uint8 {
	return uint8(math.Round(float64(a) + (float64(b)-float64(a))*t))
}

func addU8(a, b uint8) uint8 {
	s := uint16(a) + uint16(b)
	if s > 255 {
		return 255
	}
	return uint8(s)
}
