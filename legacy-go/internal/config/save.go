package config

import (
	"fmt"
	"os"
	"path/filepath"

	"github.com/BurntSushi/toml"
)

// SaveDevice writes device.toml. Used by the web config editor.
func SaveDevice(dir string, d Device) error {
	return encodeTOML(filepath.Join(dir, "device.toml"), d)
}

// SaveTheme writes theme.toml. Used by the web config editor; the caller is
// responsible for pushing the new theme to the engine for a live update.
func SaveTheme(dir string, t Theme) error {
	return encodeTOML(filepath.Join(dir, "theme.toml"), t)
}

func encodeTOML(path string, v any) error {
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return fmt.Errorf("config: mkdir for %s: %w", path, err)
	}
	f, err := os.OpenFile(path, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, 0o644)
	if err != nil {
		return fmt.Errorf("config: open %s: %w", path, err)
	}
	defer f.Close()
	if err := toml.NewEncoder(f).Encode(v); err != nil {
		return fmt.Errorf("config: encode %s: %w", path, err)
	}
	return nil
}
