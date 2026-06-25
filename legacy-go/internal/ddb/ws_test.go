package ddb

import (
	"context"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/coder/websocket"
)

type stubToken struct{}

func (stubToken) Token(context.Context) (string, error) { return "tok", nil }

func TestWSListenerNudgesOnMessage(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		// Verify the stt token made it into the query string.
		if r.URL.Query().Get("stt") != "tok" {
			t.Errorf("missing stt token: %q", r.URL.RawQuery)
		}
		c, err := websocket.Accept(w, r, nil)
		if err != nil {
			return
		}
		defer c.Close(websocket.StatusNormalClosure, "")
		_ = c.Write(r.Context(), websocket.MessageText, []byte(`{"event":"changed"}`))
		<-r.Context().Done()
	}))
	defer srv.Close()

	nudged := make(chan struct{}, 4)
	l := NewWSListener(stubToken{}, "game1", "user1")
	l.urlBase = "ws" + strings.TrimPrefix(srv.URL, "http") // http(s)->ws
	l.ReconnectBackoff = 10 * time.Millisecond
	l.OnNudge = func() { nudged <- struct{}{} }

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go l.Run(ctx)

	select {
	case <-nudged:
	case <-time.After(2 * time.Second):
		t.Fatal("expected a nudge from the websocket message")
	}
}

func TestWSListenerNoOpWithoutGame(t *testing.T) {
	done := make(chan struct{})
	l := NewWSListener(stubToken{}, "", "")
	go func() { l.Run(context.Background()); close(done) }()
	select {
	case <-done:
	case <-time.After(time.Second):
		t.Fatal("Run should return immediately when game/user unset")
	}
}
