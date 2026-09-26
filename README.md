# D&D Beyond LED Health Bar

An LED health bar that mirrors a D&D Beyond character's hit points on a WS2812B
strip — green→amber→red gradient, damage/heal flashes, and a low-HP heartbeat —
so the table can see how close a character is to going down.

A **server** does the heavy lifting (TLS to D&D Beyond, HP math, the game-log
websocket) and publishes one tiny precomputed line per character. The **device**
is a thin poller: it fetches that line over plain HTTP and drives the strip, plus
self-updates over the air.

## Two boards, one contract

The same behaviour runs on either microcontroller; pick per build:

- **Raspberry Pi Pico 2 W** (RP2350) — C firmware in [`firmware-c/`](firmware-c/).
- **ESP32-C3** (Seeed XIAO) — MicroPython firmware in [`firmware-esp32/`](firmware-esp32/).

They share the server, the wire/OTA contract, and the tooling; only the firmware
tree, toolchain, and OTA mechanism differ (hidden behind the `just` verbs below).
Firmware images are served **per board** (`/firmware/<board>/…`) so an image can
never reach the wrong architecture.

**New here? Start with [`AGENTS.md`](AGENTS.md)** — the split-architecture map,
what's shared vs per-board, and build/flash details. The device↔server spec is
[`docs/firmware-contract.md`](docs/firmware-contract.md).

## Layout

- **[`firmware-c/`](firmware-c/)** — **Pico 2 W** firmware (C / pico-sdk): thin
  poller + A/B OTA with bootrom rollback. Current Pico build.
- **[`firmware-esp32/`](firmware-esp32/)** — **ESP32-C3** firmware (MicroPython):
  thin poller + `esp32.Partition` OTA. Current ESP32 build.
- **[`server/`](server/)** — the tracker server (D&D Beyond, HP math, device feed,
  per-board OTA feed).
- **[`docs/`](docs/)** — [`firmware-contract.md`](docs/firmware-contract.md),
  [`hardware-build.md`](docs/hardware-build.md), and design docs in `docs/plans/`.
- **[`firmware/`](firmware/)** (Pico, MicroPython) and
  **[`legacy-go/`](legacy-go/)** (Pi Zero 2 W, Go) — superseded, kept for
  reference.

## Quick start

The board is the first argument to every task (`pico` | `esp32`):

```sh
just build <board>               # build firmware
just flash <board>               # flash a USB-connected board (dev)
just publish <board> <version>   # build + publish an OTA image to the server
just test                        # host unit tests for both firmwares (no hardware)
just                             # list all tasks
```

- **Pico:** `just flash pico` (loads the UF2 over USB). OTA: `just publish pico N`.
- **ESP32:** flash the MicroPython runtime once with `esptool`, then
  `just flash esp32` (copies the app + config via `mpremote`). OTA needs the
  dual-partition MicroPython build — see [`firmware-esp32/README.md`](firmware-esp32/README.md#ota).

First boot with no saved WiFi brings up a `healthbar-setup` access point with a
captive portal — connect to it to enter your WiFi and the character **slug** the
device should poll.

## Develop without hardware

```sh
just test                                          # both firmwares' host logic tests (CPython)
cd firmware-esp32 && python3 tests/test_poll.py    # or run a single tree's tests directly
```

The animation engine renders to a terminal via a `SimStrip` backend, so the LED
logic can be exercised without a board.
