package led

import (
	"fmt"
	"io"
	"os"
	"strings"
)

// Simulator renders frames to a terminal using ANSI 24-bit color, drawing the
// strip as a row of colored blocks that updates in place. It lets the full
// application run without any hardware.
type Simulator struct {
	n       int
	w       io.Writer
	glyph   string
	started bool
}

// NewSimulator returns a terminal-backed strip of n pixels writing to stdout.
func NewSimulator(n int) *Simulator {
	return &Simulator{n: n, w: os.Stdout, glyph: "██"}
}

// NewSimulatorWriter is like NewSimulator but writes to an arbitrary writer
// (used in tests).
func NewSimulatorWriter(n int, w io.Writer) *Simulator {
	return &Simulator{n: n, w: w, glyph: "██"}
}

// Len reports the pixel count.
func (s *Simulator) Len() int { return s.n }

// Render draws the frame as colored blocks on the current terminal line.
func (s *Simulator) Render(frame Frame) error {
	if len(frame) != s.n {
		return fmt.Errorf("led: frame length %d != strip length %d", len(frame), s.n)
	}
	var b strings.Builder
	b.WriteByte('\r')
	for _, c := range frame {
		fmt.Fprintf(&b, "\x1b[38;2;%d;%d;%dm%s", c.R, c.G, c.B, s.glyph)
	}
	b.WriteString("\x1b[0m")
	s.started = true
	_, err := io.WriteString(s.w, b.String())
	return err
}

// Close finishes the in-place line so subsequent output starts cleanly.
func (s *Simulator) Close() error {
	if !s.started {
		return nil
	}
	_, err := io.WriteString(s.w, "\x1b[0m\n")
	return err
}
