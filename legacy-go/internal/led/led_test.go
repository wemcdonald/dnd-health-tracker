package led

import (
	"bytes"
	"strings"
	"testing"
)

func TestScaleClamps(t *testing.T) {
	c := RGB(200, 100, 50)
	if got := c.Scale(0); got != Black {
		t.Errorf("Scale(0) = %v, want black", got)
	}
	if got := c.Scale(2); got != c {
		t.Errorf("Scale(2) clamps to 1, got %v want %v", got, c)
	}
	if got := c.Scale(0.5); got != (Color{100, 50, 25}) {
		t.Errorf("Scale(0.5) = %v", got)
	}
}

func TestLerpEndpoints(t *testing.T) {
	a, b := RGB(0, 0, 0), RGB(100, 200, 50)
	if got := Lerp(a, b, 0); got != a {
		t.Errorf("Lerp t=0 = %v want %v", got, a)
	}
	if got := Lerp(a, b, 1); got != b {
		t.Errorf("Lerp t=1 = %v want %v", got, b)
	}
	if got := Lerp(a, b, 0.5); got != (Color{50, 100, 25}) {
		t.Errorf("Lerp t=0.5 = %v", got)
	}
}

func TestAddSaturates(t *testing.T) {
	if got := RGB(200, 0, 0).Add(RGB(100, 0, 0)); got.R != 255 {
		t.Errorf("Add should saturate to 255, got %d", got.R)
	}
}

func TestFrameOps(t *testing.T) {
	f := NewFrame(4)
	f.Fill(RGB(1, 2, 3))
	c := f.Clone()
	f.Clear()
	if c[0] != (Color{1, 2, 3}) {
		t.Errorf("Clone not independent: %v", c[0])
	}
	if f[0] != Black {
		t.Errorf("Clear failed: %v", f[0])
	}
}

func TestSimulatorRender(t *testing.T) {
	var buf bytes.Buffer
	s := NewSimulatorWriter(2, &buf)
	if err := s.Render(Frame{RGB(255, 0, 0), RGB(0, 255, 0)}); err != nil {
		t.Fatal(err)
	}
	out := buf.String()
	if !strings.Contains(out, "38;2;255;0;0") || !strings.Contains(out, "38;2;0;255;0") {
		t.Errorf("simulator missing truecolor codes: %q", out)
	}
	if err := s.Render(Frame{RGB(0, 0, 0)}); err == nil {
		t.Error("expected error for wrong frame length")
	}
}
