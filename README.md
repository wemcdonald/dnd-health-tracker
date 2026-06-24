# dnd-health-tracker

A glowing WS2812B LED health bar that mirrors a D&D Beyond character's current
HP in near-real-time. Runs on a Raspberry Pi Zero 2 W; one unit per party
member. Written in Go.

See the full design in `docs/plans/` for architecture rationale, and
[`deploy/README.md`](deploy/README.md) for the hardware build + provisioning
guide.

## What it does

- **Live HP** from D&D Beyond: public-first fetch (no login needed if the sheet
  is Public), or authenticated via a pasted Cobalt cookie for private sheets.
- **Responsive without hammering**: an adaptive poller (fast right after a
  change, slow when idle, jittered) plus an optional Maps-session **websocket**
  that pushes "fetch now" nudges for near-instant updates.
- **Animations**: red/green flash on damage/heal, an eased bar adjust, an
  ambient idle shimmer, a low-HP heartbeat, and distinct boot/connecting/offline
  status patterns — each a discrete, unit-tested function; all colors/timing are
  hot-reloadable constants in `theme.toml`.
- **No-auth web UI**: status + editors for player, theme (live), and WiFi, plus
  a credential-capture flow (a bookmarklet grabs the character id; guided paste
  for the cookie).
- **Bulletproof WiFi**: multiple known networks synced into NetworkManager with
  priority, and a captive-portal access-point fallback (`healthbar-setup`) when
  no known network is in range.
- **Runs headless** on the Pi via systemd; SSH-over-USB-C for maintenance.

## Architecture

A single Go binary. Goroutines share a mutex-guarded animation `Engine`:
the **poller**/**websocket** (`internal/ddb`) push HP + status into it, a
fixed-FPS **render loop** reads it and drives the **LED strip** (`internal/led`,
hardware or terminal simulator), the **web UI** (`internal/web`) edits config
through the **app supervisor** (`internal/app`), and `internal/netcfg` manages
WiFi. The whole thing runs on a laptop with the simulator — no hardware needed.

## Develop without hardware

The entire app runs on a laptop using the terminal **simulator** backend — no
Pi or LED strip required.

```sh
# A live HP bar that ramps up and down:
go run ./cmd/healthbar --demo

# A static bar at 50% HP with 10% temporary HP:
go run ./cmd/healthbar --hp 0.5 --temp 0.1

# Use example config instead of /etc/healthbar:
go run ./cmd/healthbar --demo --config-dir ./deploy/config
```

`--sim` is the default. Press Ctrl-C to exit (the strip is blanked on exit).

## Build for the Raspberry Pi

The hardware backend wraps the jgarff `rpi_ws281x` C library and is compiled
only with the `hw` build tag (so laptop/CI builds need neither cgo nor the C
library):

```sh
go build -tags hw ./cmd/healthbar   # on the Pi, with rpi_ws281x installed
```

Cross-compilation and provisioning are covered in `deploy/` (added in a later
phase).

## Configuration

Two simple TOML files (hand-editable and written by the web UI). Examples live
in `deploy/config/`:

- `device.toml` — player name, D&D Beyond character id, LED count, GPIO pin,
  brightness, polling cadence.
- `theme.toml` — all colors, thresholds, and animation timing.

Missing files fall back to built-in defaults, so a fresh unit boots into a sane
state before it is configured.

## Tests

```sh
go test ./...
```
