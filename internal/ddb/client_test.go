package ddb

import (
	"context"
	"errors"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"testing"
)

func TestFetchHPPublic(t *testing.T) {
	body, err := os.ReadFile(filepath.Join("..", "..", "testdata", "character_public.json"))
	if err != nil {
		t.Fatal(err)
	}
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/12345" {
			t.Errorf("unexpected path %q", r.URL.Path)
		}
		w.Header().Set("Content-Type", "application/json")
		w.Write(body)
	}))
	defer srv.Close()

	c := NewClient().WithBase(srv.URL + "/")
	hp, err := c.FetchHP(context.Background(), "12345")
	if err != nil {
		t.Fatal(err)
	}
	if hp != (HP{Current: 40, Max: 53, Temp: 0}) {
		t.Errorf("got %+v", hp)
	}
}

func TestFetchHPPrivateReturnsAuthError(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusForbidden)
	}))
	defer srv.Close()

	c := NewClient().WithBase(srv.URL + "/")
	_, err := c.FetchHP(context.Background(), "1")
	var se *StatusError
	if !errors.As(err, &se) || !se.RequiresAuth() {
		t.Fatalf("expected auth StatusError, got %v", err)
	}
}

func TestFetchHPUnsuccessfulEnvelope(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Write([]byte(`{"success": false, "message": "not found", "data": null}`))
	}))
	defer srv.Close()

	c := NewClient().WithBase(srv.URL + "/")
	if _, err := c.FetchHP(context.Background(), "1"); err == nil {
		t.Error("expected error for unsuccessful envelope")
	}
}

func TestFetchHPEmptyID(t *testing.T) {
	if _, err := NewClient().FetchHP(context.Background(), ""); err == nil {
		t.Error("expected error for empty character id")
	}
}
