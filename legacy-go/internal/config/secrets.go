package config

import (
	"fmt"
	"os"
	"path/filepath"

	"github.com/BurntSushi/toml"
)

// secretsFile is the filename (mode 0600) holding the D&D Beyond session
// cookie for private sheets. Absent for public-only setups.
const secretsFile = "secrets.toml"

// Secrets holds credentials kept out of the main config files and out of git.
// Only the Cobalt session cookie is durable; short-lived tokens are derived
// from it at runtime and never persisted.
type Secrets struct {
	// CobaltCookie is the raw Cookie header value copied from a logged-in
	// D&D Beyond browser session (contains CobaltSession=...). Empty means the
	// public, no-credential path.
	CobaltCookie string `toml:"cobalt_cookie"`
}

// LoadSecrets reads secrets.toml from dir. A missing file yields empty Secrets
// (public path); a present-but-invalid file is an error.
func LoadSecrets(dir string) (Secrets, error) {
	var s Secrets
	path := filepath.Join(dir, secretsFile)
	if _, err := os.Stat(path); os.IsNotExist(err) {
		return s, nil
	}
	if _, err := toml.DecodeFile(path, &s); err != nil {
		return s, fmt.Errorf("config: decode %s: %w", path, err)
	}
	return s, nil
}

// SaveSecrets writes secrets.toml with 0600 permissions (owner-only), since it
// contains a credential. Used by the web UI's credential-capture flow.
func SaveSecrets(dir string, s Secrets) error {
	path := filepath.Join(dir, secretsFile)
	f, err := os.OpenFile(path, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, 0o600)
	if err != nil {
		return fmt.Errorf("config: open %s: %w", path, err)
	}
	defer f.Close()
	if err := toml.NewEncoder(f).Encode(s); err != nil {
		return fmt.Errorf("config: encode secrets: %w", err)
	}
	return nil
}
