package ddb

import (
	"context"
	"errors"
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

type stubFetcher struct {
	mu    sync.Mutex
	calls int
	fn    func(n int) (HP, error)
}

func (s *stubFetcher) FetchHP(_ context.Context, _ string) (HP, error) {
	s.mu.Lock()
	n := s.calls
	s.calls++
	s.mu.Unlock()
	return s.fn(n)
}

func fastPoller(stub fetcher) *Poller {
	return &Poller{
		Fetcher:     stub,
		CharacterID: "1",
		Fast:        5 * time.Millisecond,
		Idle:        5 * time.Millisecond,
		FastWindow:  5 * time.Millisecond,
		ErrBackoff:  5 * time.Millisecond,
	}
}

func TestPollerEmitsOnlyOnChange(t *testing.T) {
	var hpCalls, onlineCalls int32
	p := fastPoller(&stubFetcher{fn: func(int) (HP, error) { return HP{40, 53, 0}, nil }})
	p.OnHP = func(HP) { atomic.AddInt32(&hpCalls, 1) }
	p.OnStatus = func(o bool) {
		if o {
			atomic.AddInt32(&onlineCalls, 1)
		}
	}
	ctx, cancel := context.WithTimeout(context.Background(), 80*time.Millisecond)
	defer cancel()
	p.Run(ctx)

	if got := atomic.LoadInt32(&hpCalls); got != 1 {
		t.Errorf("constant HP should emit OnHP once, got %d", got)
	}
	if got := atomic.LoadInt32(&onlineCalls); got < 2 {
		t.Errorf("expected repeated online status, got %d", got)
	}
}

func TestPollerEmitsEveryChange(t *testing.T) {
	var hpCalls int32
	// Each fetch returns a different current HP.
	p := fastPoller(&stubFetcher{fn: func(n int) (HP, error) { return HP{40 - n, 53, 0}, nil }})
	p.OnHP = func(HP) { atomic.AddInt32(&hpCalls, 1) }
	ctx, cancel := context.WithTimeout(context.Background(), 80*time.Millisecond)
	defer cancel()
	p.Run(ctx)

	if got := atomic.LoadInt32(&hpCalls); got < 3 {
		t.Errorf("changing HP should emit OnHP repeatedly, got %d", got)
	}
}

func TestPollerHandlesErrorsThenRecovers(t *testing.T) {
	var offline, online, hpCalls int32
	p := fastPoller(&stubFetcher{fn: func(n int) (HP, error) {
		if n == 0 {
			return HP{}, errors.New("boom")
		}
		return HP{40, 53, 0}, nil
	}})
	p.OnHP = func(HP) { atomic.AddInt32(&hpCalls, 1) }
	p.OnStatus = func(o bool) {
		if o {
			atomic.AddInt32(&online, 1)
		} else {
			atomic.AddInt32(&offline, 1)
		}
	}
	ctx, cancel := context.WithTimeout(context.Background(), 80*time.Millisecond)
	defer cancel()
	p.Run(ctx)

	if atomic.LoadInt32(&offline) < 1 {
		t.Error("expected an offline status after the error")
	}
	if atomic.LoadInt32(&online) < 1 || atomic.LoadInt32(&hpCalls) < 1 {
		t.Error("expected recovery: online status and an HP emission")
	}
}

func TestPollerStopsOnContextCancel(t *testing.T) {
	p := fastPoller(&stubFetcher{fn: func(int) (HP, error) { return HP{1, 1, 0}, nil }})
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() { p.Run(ctx); close(done) }()
	cancel()
	select {
	case <-done:
	case <-time.After(time.Second):
		t.Fatal("Run did not return after cancel")
	}
}
