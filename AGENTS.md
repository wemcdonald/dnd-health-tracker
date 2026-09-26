# AGENTS.md — orientation for this repo

D&D Beyond LED health bar. A device mirrors a character's HP on a WS2812B strip.
The heavy lifting (TLS to D&D Beyond, HP math, the game-log websocket) lives on
the **server**; the device is a **thin poller** that fetches one precomputed line
over plain HTTP and drives the LEDs, plus self-update OTA.

## Split architecture: two boards, one contract

The same device behaviour runs on two microcontrollers. They share the server and
the wire/OTA contract, but each keeps its own native firmware (different language
+ OTA mechanism) — we deliberately did **not** force a shared-source rewrite,
because both native OTA paths are hardware-proven and rewriting them would risk
the thing that matters most: reliable field updates.

| Tree | Board | Language | OTA mechanism | Status |
|------|-------|----------|---------------|--------|
| [`firmware-c/`](firmware-c/) | Pico 2 W (RP2350) | C / pico-sdk | RP2350 bootrom **TBYB** A/B + rollback | **current Pico** |
| [`firmware-esp32/`](firmware-esp32/) | ESP32-C3 (XIAO) | MicroPython | `esp32.Partition` ota_0/1 + rollback | **current ESP32** |
| [`firmware/`](firmware/) | Pico 2 W | MicroPython | none | legacy (pre-thin-poller, DDB-direct); reference only |
| [`legacy-go/`](legacy-go/) | Pi Zero 2 W | Go | none | original; reference only |

**Authoritative spec both current firmwares implement:**
[`docs/firmware-contract.md`](docs/firmware-contract.md) — HP wire format
(`<cur> <max> <temp> <age>`), per-board OTA, version lockstep, config schema,
board IDs (`pico`, `esp32`). If code and that doc disagree, fix one of them.

## What's shared vs. per-board

- **Shared:** the server (`server/`), the OTA feed contract, the config schema,
  and the tooling (`justfile` + `server/tools/publish-fw.mjs`).
- **Per-board:** the firmware source tree, toolchain, flasher, and OTA slot
  mechanism — all hidden behind the `just` verbs below.
- **Server OTA is namespaced per board:** `GET /firmware/<board>/latest` +
  `/firmware/<board>/image.bin`, board allow-listed, so a Pico image can never be
  served to an ESP32 (which would brick it). Served over plain HTTP, no auth.

## Build / flash / publish — one interface

The board is the first argument (`pico` | `esp32`). See `justfile` for details.

```sh
just build <board> [version]     # build firmware (dev build)
just flash <board>               # flash a USB-connected board (dev; not OTA)
just publish <board> [version]   # build an OTA image + publish to the server feed
just set name <slug>             # provision slug over USB (pico)
just test                        # host unit tests for both firmwares (no hardware)
```

### Pico 2 W (`firmware-c`, C)
- Deps: `pico-sdk` (`PICO_SDK_PATH`), `picotool`, `cmake`.
- Flash (dev): `just flash pico` (loads the UF2 over USB, no BOOTSEL).
- OTA: `just publish pico <version>` (builds TBYB-flagged, publishes to
  `/firmware/pico/`). First-time A/B conversion: `just migrate-ota` (see
  [`firmware-c/ota/MIGRATION.md`](firmware-c/ota/MIGRATION.md)).

### ESP32-C3 (`firmware-esp32`, MicroPython)
1. Flash the **MicroPython runtime** once with `esptool` (stock build for dev; a
   **dual-OTA-partition, rollback-enabled** build is required for OTA).
2. `just flash esp32` copies the app `.py` + `data/config.json` via `mpremote`.
3. OTA: `just publish esp32 <version>` (needs a MicroPython image build — see
   [`firmware-esp32/README.md`](firmware-esp32/README.md#ota); freeze set in
   `firmware-esp32/frozen_manifest.py`). Publishes to `/firmware/esp32/`.

Both boards: first boot with no WiFi/slug → `healthbar-setup` AP + captive portal
to enter WiFi + character slug (or pre-seed `data/config.json`).

## Hardware

[`docs/hardware-build.md`](docs/hardware-build.md) — wiring/power for both boards
(Pico + LiPo SHIM/USB-C breakout; ESP32-C3 with external charger board + power
switch on OUT). ESP32 LED data = **pad D1 / GPIO3** (`gpio_pin: 3`).

## Testing

- Firmware host logic (CPython, no hardware): `just test`, or per tree
  `python3 tests/test_*.py`.
- Server: `cd server && npx vitest run` (+ `npm run typecheck`).
- On-hardware behaviour (WiFi join, neopixel timing, OTA flash/rollback) is
  **not** host-testable — treat first flashes on new hardware as bring-up.

## Server / deploy

Runs as the `dnd-health` container in the willflix compose (`/willflix/docker/
compose.yml`), behind Traefik: admin UI is HTTPS + Authentik; `*.txt` device feed,
`/firmware/**`, and `/device/**` (the board config channel — see
[`server/src/routes/device.ts`](server/src/routes/device.ts)) are plain HTTP, no
auth (the device has no TLS). The Traefik router rule in `/willflix/docker/
compose.yml` must include `PathPrefix(`/device/`)` on that same plain-HTTP
router, not behind Authentik — without it the board can never register (its
check-ins never reach the app) and it silently never shows up in the admin UI.

**How an OTA image reaches a device:** `just publish <board> <version>` runs
`server/tools/publish-fw.mjs`, which writes `server/firmware/<board>/{image.bin,
manifest.txt}`. Despite `server/.gitignore` ignoring `firmware/*` generally, it
explicitly un-ignores `firmware/*/image.bin` and `firmware/*/manifest.txt` — so
these two files per board **are tracked in git** (verified: `firmware/pico/
image.bin` and `manifest.txt` are both committed, two commits deep). That's
deliberate: git history is the firmware changelog, and it's also the deploy
transport. The willflix container bind-mounts `server/firmware` (see `volumes:`
in `server/docker-compose.yml`) and `server/src/routes/firmware.ts` reads the
files straight off disk on every request (no caching), so deploying a published
image is: commit + push the two files from your `just publish` run, then `git
pull` on willflix — no rebuild or restart needed, the next `/firmware/<board>/
latest` or `image.bin` request serves the new bytes immediately.
