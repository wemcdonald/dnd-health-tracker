package ddb

import (
	"context"
	"math/rand"
	"time"
)

// fetcher is the subset of Client the poller needs (so tests can stub it).
type fetcher interface {
	FetchHP(ctx context.Context, characterID string) (HP, error)
}

// Poller repeatedly fetches a character's HP. It polls quickly for a window
// after each detected change (so damage/healing shows within seconds) and
// backs off to a slow idle cadence otherwise (so D&D Beyond is not hammered).
// Errors trigger exponential backoff and an offline status callback.
type Poller struct {
	Fetcher     fetcher
	CharacterID string

	Fast       time.Duration // cadence during an active window
	Idle       time.Duration // cadence when nothing is changing
	FastWindow time.Duration // how long to stay in fast mode after a change
	ErrBackoff time.Duration // base error backoff, doubled per consecutive error

	// OnHP is called only when the snapshot differs from the previous one.
	OnHP func(HP)
	// OnStatus reports reachability (true after a good fetch, false on error).
	OnStatus func(online bool)

	rng *rand.Rand
}

// Run polls until ctx is cancelled. It blocks; run it in a goroutine.
func (p *Poller) Run(ctx context.Context) {
	p.applyDefaults()

	var last HP
	have := false
	fastUntil := time.Time{}
	errCount := 0

	for {
		hp, err := p.Fetcher.FetchHP(ctx, p.CharacterID)
		if err != nil {
			p.status(false)
			errCount++
			if !sleepCtx(ctx, p.errorBackoff(errCount)) {
				return
			}
			continue
		}
		errCount = 0
		p.status(true)

		if !have || hp != last {
			last, have = hp, true
			if p.OnHP != nil {
				p.OnHP(hp)
			}
			fastUntil = time.Now().Add(p.FastWindow)
		}

		interval := p.Idle
		if time.Now().Before(fastUntil) {
			interval = p.Fast
		}
		if !sleepCtx(ctx, p.jitter(interval)) {
			return
		}
	}
}

func (p *Poller) applyDefaults() {
	if p.Fast <= 0 {
		p.Fast = 3 * time.Second
	}
	if p.Idle <= 0 {
		p.Idle = 25 * time.Second
	}
	if p.FastWindow <= 0 {
		p.FastWindow = 20 * time.Second
	}
	if p.ErrBackoff <= 0 {
		p.ErrBackoff = 2 * time.Second
	}
	if p.rng == nil {
		p.rng = rand.New(rand.NewSource(time.Now().UnixNano()))
	}
}

func (p *Poller) status(online bool) {
	if p.OnStatus != nil {
		p.OnStatus(online)
	}
}

// jitter adds +/-15% so a fleet of bars doesn't poll in lockstep.
func (p *Poller) jitter(d time.Duration) time.Duration {
	delta := (p.rng.Float64()*0.3 - 0.15) * float64(d)
	return d + time.Duration(delta)
}

// errorBackoff doubles the base backoff per consecutive error, capped at 60s.
func (p *Poller) errorBackoff(n int) time.Duration {
	d := p.ErrBackoff << uint(min(n-1, 5))
	if d > 60*time.Second {
		d = 60 * time.Second
	}
	return p.jitter(d)
}

// sleepCtx sleeps for d or until ctx is done; returns false if ctx ended.
func sleepCtx(ctx context.Context, d time.Duration) bool {
	if d <= 0 {
		d = time.Millisecond
	}
	t := time.NewTimer(d)
	defer t.Stop()
	select {
	case <-ctx.Done():
		return false
	case <-t.C:
		return true
	}
}

func min(a, b int) int {
	if a < b {
		return a
	}
	return b
}
