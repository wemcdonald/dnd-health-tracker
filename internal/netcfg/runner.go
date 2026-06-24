// Package netcfg manages WiFi on the Raspberry Pi via NetworkManager (nmcli):
// it syncs the known-networks list into NM connection profiles and runs a
// captive-portal access-point fallback when no known network is reachable.
//
// All NetworkManager interaction goes through the Runner interface so the
// command-building logic is unit-testable without a real nmcli.
package netcfg

import (
	"context"
	"os/exec"
)

// Runner executes an external command and returns its combined output.
type Runner interface {
	Run(ctx context.Context, name string, args ...string) (string, error)
}

// ExecRunner runs commands with os/exec (the real implementation on the Pi).
type ExecRunner struct{}

// Run executes name with args and returns combined stdout+stderr.
func (ExecRunner) Run(ctx context.Context, name string, args ...string) (string, error) {
	out, err := exec.CommandContext(ctx, name, args...).CombinedOutput()
	return string(out), err
}
