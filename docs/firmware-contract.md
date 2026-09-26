# Firmware ↔ server contract

The single spec every device firmware implements, regardless of board. Both the
Pico (`firmware-c`, C) and the ESP32 (`firmware-esp32`, MicroPython) builds must
behave identically against this; the server (`server/`) is the other side of it.
Keep this doc authoritative — if code and this doc disagree, fix one of them.

Transport for everything below is **plain HTTP** (port 80), **no auth**, no TLS on
the device. Only these paths are exposed over plain HTTP; the admin UI stays
HTTPS + auth.

## Board IDs

`pico` (Raspberry Pi Pico 2 W / RP2350), `esp32` (ESP32-C3). The server
allow-lists these; add new IDs in `server/src/routes/firmware.ts`,
`server/tools/publish-fw.mjs`, and the `justfile`.

## 1. Device HP feed

`GET /<slug>.txt` → `text/plain`. The device polls this every `poll_seconds`.

**Line 1 is four space-separated integers:** `<cur> <max> <temp> <age>`

| field | meaning |
|-------|---------|
| `cur` | current HP (0..max) |
| `max` | max HP (≥ 1 for a live character; `0` is the "no data" sentinel) |
| `temp` | temporary HP (separate buffer on top of `cur`) |
| `age` | seconds since the server last refreshed from D&D Beyond (`99999` = never) |

Line 2+ is human-readable and **ignored** by the device.

The device treats the feed as **offline** (breathing animation) if the response
is non-200, unparseable, has fewer than 4 ints, or has `max < 1`. So a 404, a
garbage body, or the `0 0 0 99999` sentinel all degrade safely. On a valid line
the device maps `cur/max` (+`temp`) to the bar and shows online.

> Historical note: an earlier `WIRE_FORMAT.md` documented a 2-int `<lit> <age>`
> line where the server precomputed the lit count. That is **obsolete** — the
> server now sends raw `cur max temp age` and the device does the bar mapping.

## 2. OTA update (device-initiated pull, per board)

Images are namespaced per board so one architecture's image can never reach
another:

- `GET /firmware/<board>/latest` → manifest, `text/plain`:
  ```
  <version> <size>
  <sha256>
  <imagePath>
  ```
  `version` monotonic int; `size` bytes; `sha256` 64 lowercase hex of the image;
  `imagePath` absolute, `/firmware/<board>/image.bin`.
- `GET /firmware/<board>/image.bin` → the raw image (`application/octet-stream`,
  `Accept-Ranges: bytes`; supports `bytes=START-END` / `bytes=START-` ranges).

**Device flow (identical on both boards):** fetch the manifest; if
`version > FIRMWARE_VERSION`, stream the image into the inactive slot while
hashing; verify streamed size + SHA-256 against the manifest; stage + reboot;
after a healthy first poll, **commit** (else the bootloader rolls back). The
mechanism differs per board but the policy above does not:

| board | slots | commit / rollback |
|-------|-------|-------------------|
| pico  | RP2350 A/B partitions | bootrom **TBYB** trial flag; commit clears it |
| esp32 | `esp32.Partition` ota_0/ota_1 | `set_boot` + `mark_app_valid_cancel_rollback` |

**Versioning:** the published manifest `version` MUST equal the `FIRMWARE_VERSION`
baked into that image (`firmware-c` `FIRMWARE_VERSION`, `firmware-esp32/version.py`).
`just publish <board> <version>` passes the same number to both, keeping them in
lockstep. The device only updates on strictly-newer.

## 3. Device config (on-device flash, per unit)

`data/config.json` — identity + hardware, persists across OTA (it's on the
device filesystem, not in the image):

| field | meaning | notes |
|-------|---------|-------|
| `player_name` | label | optional |
| `slug` | character slug → polls `/<slug>.txt` | required for RUN mode |
| `server_host` | server hostname | default `dndhealth.willflix.org` |
| `server_port` | port | default `80` |
| `num_leds` | strip length | default `16` |
| `gpio_pin` | WS2812 data pin (chip GPIO number) | pico default `18`; esp32 default `3` (pad D1) |
| `brightness` | 0..1 hardware brightness | |
| `poll_seconds` | HP poll interval | |

No character/user/game IDs or Cobalt cookie on-device — all D&D Beyond work is
server-side. First boot with no WiFi/slug → `healthbar-setup` AP + captive portal.

## Build / serve / update

One interface, board as the argument (see `justfile`):
`just build <board>`, `just flash <board>`, `just publish <board> <version>`.
`publish` builds the image, stamps the version, and writes a correctly-formatted
manifest into `server/firmware/<board>/` via `server/tools/publish-fw.mjs`.
