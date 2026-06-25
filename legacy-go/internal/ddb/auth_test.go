package ddb

import (
	"context"
	"errors"
	"net/http"
	"net/http/httptest"
	"sync/atomic"
	"testing"
	"time"
)

func TestCookieAuthFetchesAndCaches(t *testing.T) {
	var hits int32
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		atomic.AddInt32(&hits, 1)
		if r.Header.Get("Cookie") != "CobaltSession=secret" {
			t.Errorf("cookie not forwarded: %q", r.Header.Get("Cookie"))
		}
		w.Write([]byte(`{"token":"abc123","ttl":300}`))
	}))
	defer srv.Close()

	a := NewCookieAuth("CobaltSession=secret")
	a.authURL = srv.URL

	tok, err := a.Token(context.Background())
	if err != nil || tok != "abc123" {
		t.Fatalf("Token = %q, %v", tok, err)
	}
	// Second call should be served from cache (no extra request).
	if _, err := a.Token(context.Background()); err != nil {
		t.Fatal(err)
	}
	if got := atomic.LoadInt32(&hits); got != 1 {
		t.Errorf("expected token cached after 1 request, got %d requests", got)
	}
}

func TestCookieAuthRefreshesWhenExpired(t *testing.T) {
	var hits int32
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		atomic.AddInt32(&hits, 1)
		w.Write([]byte(`{"token":"abc","ttl":300}`))
	}))
	defer srv.Close()

	a := NewCookieAuth("c")
	a.authURL = srv.URL
	if _, err := a.Token(context.Background()); err != nil {
		t.Fatal(err)
	}
	a.mu.Lock()
	a.expires = time.Now().Add(-time.Minute) // force expiry
	a.mu.Unlock()
	if _, err := a.Token(context.Background()); err != nil {
		t.Fatal(err)
	}
	if got := atomic.LoadInt32(&hits); got != 2 {
		t.Errorf("expected a refresh request, got %d total", got)
	}
}

func TestCookieAuthAuthorizeSetsHeaders(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Write([]byte(`{"token":"tkn","ttl":300}`))
	}))
	defer srv.Close()
	a := NewCookieAuth("CobaltSession=xyz")
	a.authURL = srv.URL

	req, _ := http.NewRequestWithContext(context.Background(), http.MethodGet, "https://example.com", nil)
	if err := a.Authorize(req); err != nil {
		t.Fatal(err)
	}
	if req.Header.Get("Authorization") != "Bearer tkn" {
		t.Errorf("authorization = %q", req.Header.Get("Authorization"))
	}
	if req.Header.Get("Cookie") != "CobaltSession=xyz" {
		t.Errorf("cookie = %q", req.Header.Get("Cookie"))
	}
}

func TestCookieAuthErrors(t *testing.T) {
	if _, err := NewCookieAuth("").Token(context.Background()); err == nil {
		t.Error("empty cookie should error")
	}
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusForbidden)
	}))
	defer srv.Close()
	a := NewCookieAuth("c")
	a.authURL = srv.URL
	_, err := a.Token(context.Background())
	var se *StatusError
	if !errors.As(err, &se) {
		t.Errorf("expected StatusError, got %v", err)
	}
}
