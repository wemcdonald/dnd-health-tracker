# dnd-health-tracker

A glowing WS2812B LED health bar that mirrors a D&D Beyond character's current
HP in near-real-time. Runs on a Raspberry Pi Zero 2 W; one unit per party
member. Written in Go.

See the full design in `docs/plans/` (or the approved plan) for architecture,
D&D Beyond integration, WiFi/captive-portal, and provisioning details.

## Status

Built in phases. **Phase 1 (skeleton + simulator)** is complete: config
loading, the LED strip abstraction, a terminal simulator backend, and the
HP→bar mapping. Remaining phases add animations + state machine, the D&D Beyond
client (poll + websocket), the no-auth web config UI, multi-network WiFi with a
captive-portal fallback, and on-Pi hardware/provisioning.

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
