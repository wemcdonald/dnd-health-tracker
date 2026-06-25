package ddb

import (
	"context"
	"fmt"
	"net/http"
	"net/url"
	"time"

	"github.com/coder/websocket"
)

const wsURLBase = "wss://game-log-api-live.dndbeyond.com/v1"

// tokenProvider yields a valid bearer token (satisfied by *CookieAuth).
type tokenProvider interface {
	Token(ctx context.Context) (string, error)
}

// WSListener subscribes to a D&D Beyond Maps game-log websocket and calls
// OnNudge on every received event. The listener treats the socket purely as a
// "something changed, fetch now" signal — it does not parse event bodies — so
// it is robust to the unofficial event schema changing. It reconnects with
// backoff and refreshes the token on each (re)connect. Requires a live Maps
// session; the poller remains the reliable baseline.
type WSListener struct {
	Token  tokenProvider
	GameID string
	UserID string

	// OnNudge is invoked once per received message (e.g. to trigger a poll).
	OnNudge func()

	HTTP             *http.Client
	urlBase          string
	ReconnectBackoff time.Duration
}

// NewWSListener builds a listener for a game/user using the given token source.
func NewWSListener(tp tokenProvider, gameID, userID string) *WSListener {
	return &WSListener{
		Token:            tp,
		GameID:           gameID,
		UserID:           userID,
		urlBase:          wsURLBase,
		ReconnectBackoff: 5 * time.Second,
	}
}

// Run maintains the websocket connection until ctx is cancelled. Blocking; run
// it in a goroutine. It is a no-op (returns immediately) if GameID/UserID are
// unset, so callers can start it unconditionally.
func (w *WSListener) Run(ctx context.Context) {
	if w.GameID == "" || w.UserID == "" {
		return
	}
	for {
		if err := w.connectOnce(ctx); err != nil && ctx.Err() == nil {
			// Connection failed or dropped; wait then retry.
		}
		if !sleepCtx(ctx, w.ReconnectBackoff) {
			return
		}
	}
}

// connectOnce dials, then reads messages (nudging on each) until the socket
// closes or ctx is cancelled.
func (w *WSListener) connectOnce(ctx context.Context) error {
	tok, err := w.Token.Token(ctx)
	if err != nil {
		return fmt.Errorf("ddb: ws token: %w", err)
	}
	u := fmt.Sprintf("%s?%s", w.urlBase, url.Values{
		"gameId": {w.GameID},
		"userId": {w.UserID},
		"stt":    {tok},
	}.Encode())

	c, _, err := websocket.Dial(ctx, u, &websocket.DialOptions{HTTPClient: w.HTTP})
	if err != nil {
		return fmt.Errorf("ddb: ws dial: %w", err)
	}
	defer c.Close(websocket.StatusNormalClosure, "")
	c.SetReadLimit(1 << 20)

	for {
		if _, _, err := c.Read(ctx); err != nil {
			return err
		}
		if w.OnNudge != nil {
			w.OnNudge()
		}
	}
}
