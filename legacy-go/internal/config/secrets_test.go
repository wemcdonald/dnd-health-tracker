package config

import (
	"os"
	"path/filepath"
	"testing"
)

func TestSecretsMissingIsEmpty(t *testing.T) {
	s, err := LoadSecrets(t.TempDir())
	if err != nil || s.CobaltCookie != "" {
		t.Errorf("missing secrets should be empty, got %+v err=%v", s, err)
	}
}

func TestSecretsRoundTripAndPerms(t *testing.T) {
	dir := t.TempDir()
	if err := SaveSecrets(dir, Secrets{CobaltCookie: "CobaltSession=abc"}); err != nil {
		t.Fatal(err)
	}
	info, err := os.Stat(filepath.Join(dir, secretsFile))
	if err != nil {
		t.Fatal(err)
	}
	if perm := info.Mode().Perm(); perm != 0o600 {
		t.Errorf("secrets file mode = %o, want 600", perm)
	}
	s, err := LoadSecrets(dir)
	if err != nil {
		t.Fatal(err)
	}
	if s.CobaltCookie != "CobaltSession=abc" {
		t.Errorf("round trip = %q", s.CobaltCookie)
	}
}
