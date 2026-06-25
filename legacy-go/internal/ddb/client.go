package ddb

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"time"
)

const (
	characterURLBase = "https://character-service.dndbeyond.com/character/v5/character/"
	originHeader     = "https://www.dndbeyond.com"
	userAgent        = "dnd-health-tracker/1.0 (+https://github.com/will/dnd-health-tracker)"
)

// StatusError is returned for non-2xx HTTP responses. 401/403 indicate the
// sheet is private and an authorizer (cookie/token) is required.
type StatusError struct {
	Code int
}

func (e *StatusError) Error() string { return fmt.Sprintf("ddb: http status %d", e.Code) }

// RequiresAuth reports whether the failure is an authentication problem (the
// signal Phase 4 uses to fall back from the public path to token auth).
func (e *StatusError) RequiresAuth() bool {
	return e.Code == http.StatusUnauthorized || e.Code == http.StatusForbidden
}

// Authorizer decorates a request with credentials (cookie + bearer token).
// Phase 4 supplies a real implementation; a nil authorizer means the public,
// unauthenticated path.
type Authorizer interface {
	Authorize(req *http.Request) error
}

// Client fetches character HP from the D&D Beyond character service.
type Client struct {
	HTTP *http.Client
	base string
	auth Authorizer
}

// NewClient returns a client pointed at the live character service.
func NewClient() *Client {
	return &Client{HTTP: &http.Client{Timeout: 15 * time.Second}, base: characterURLBase}
}

// WithBase overrides the endpoint base (used in tests with httptest). The base
// must end with a trailing slash; the character id is appended.
func (c *Client) WithBase(base string) *Client {
	c.base = base
	return c
}

// SetAuthorizer installs credentials for private sheets. Nil keeps the public path.
func (c *Client) SetAuthorizer(a Authorizer) { c.auth = a }

// FetchHP retrieves and computes the HP snapshot for a character id.
func (c *Client) FetchHP(ctx context.Context, characterID string) (HP, error) {
	if characterID == "" {
		return HP{}, fmt.Errorf("ddb: empty character id")
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, c.base+characterID, nil)
	if err != nil {
		return HP{}, err
	}
	req.Header.Set("Accept", "application/json, text/plain, */*")
	req.Header.Set("Origin", originHeader)
	req.Header.Set("Referer", originHeader+"/")
	req.Header.Set("User-Agent", userAgent)
	if c.auth != nil {
		if err := c.auth.Authorize(req); err != nil {
			return HP{}, fmt.Errorf("ddb: authorize: %w", err)
		}
	}

	resp, err := c.HTTP.Do(req)
	if err != nil {
		return HP{}, fmt.Errorf("ddb: request: %w", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		io.Copy(io.Discard, io.LimitReader(resp.Body, 4096))
		return HP{}, &StatusError{Code: resp.StatusCode}
	}

	var env apiResponse
	if err := json.NewDecoder(resp.Body).Decode(&env); err != nil {
		return HP{}, fmt.Errorf("ddb: decode: %w", err)
	}
	if !env.Success || env.Data == nil {
		return HP{}, fmt.Errorf("ddb: unsuccessful response: %s", env.Message)
	}
	return computeHP(env.Data), nil
}
