package ddb

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"sync"
	"time"
)

const (
	cobaltTokenURL = "https://auth-service.dndbeyond.com/v1/cobalt-token"
	tokenSafety    = 30 * time.Second // refresh this long before actual expiry
)

// CookieAuth turns a durable D&D Beyond Cobalt session cookie into the
// short-lived bearer ("stt") token used by the character service and websocket.
// It caches the token and refreshes it before expiry. CookieAuth implements
// Authorizer, so it can be attached to a Client for private-sheet fetches.
type CookieAuth struct {
	HTTP    *http.Client
	authURL string
	cookie  string

	mu      sync.Mutex
	token   string
	expires time.Time
}

// NewCookieAuth builds an authorizer from a raw Cookie header value.
func NewCookieAuth(cookie string) *CookieAuth {
	return &CookieAuth{
		HTTP:    &http.Client{Timeout: 15 * time.Second},
		authURL: cobaltTokenURL,
		cookie:  cookie,
	}
}

// Token returns a currently-valid bearer token, fetching or refreshing as
// needed.
func (a *CookieAuth) Token(ctx context.Context) (string, error) {
	a.mu.Lock()
	defer a.mu.Unlock()
	if a.token != "" && time.Now().Before(a.expires) {
		return a.token, nil
	}
	if err := a.refreshLocked(ctx); err != nil {
		return "", err
	}
	return a.token, nil
}

// Authorize sets the Cookie and bearer Authorization headers on req.
func (a *CookieAuth) Authorize(req *http.Request) error {
	tok, err := a.Token(req.Context())
	if err != nil {
		return err
	}
	req.Header.Set("Cookie", a.cookie)
	req.Header.Set("Authorization", "Bearer "+tok)
	return nil
}

// refreshLocked exchanges the cookie for a fresh token. Caller holds a.mu.
func (a *CookieAuth) refreshLocked(ctx context.Context) error {
	if a.cookie == "" {
		return fmt.Errorf("ddb: no cobalt cookie configured")
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, a.authURL, nil)
	if err != nil {
		return err
	}
	req.Header.Set("Accept", "*/*")
	req.Header.Set("Origin", originHeader)
	req.Header.Set("Referer", originHeader+"/")
	req.Header.Set("User-Agent", userAgent)
	req.Header.Set("Cookie", a.cookie)

	resp, err := a.HTTP.Do(req)
	if err != nil {
		return fmt.Errorf("ddb: cobalt-token request: %w", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return &StatusError{Code: resp.StatusCode}
	}

	var body struct {
		Token string `json:"token"`
		TTL   int    `json:"ttl"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&body); err != nil {
		return fmt.Errorf("ddb: decode cobalt-token: %w", err)
	}
	if body.Token == "" {
		return fmt.Errorf("ddb: cobalt-token response had no token")
	}
	ttl := time.Duration(body.TTL) * time.Second
	if ttl <= tokenSafety {
		ttl = 5 * time.Minute // sane default if ttl missing/small
	}
	a.token = body.Token
	a.expires = time.Now().Add(ttl - tokenSafety)
	return nil
}
