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
`version > FIRMWARE_VERSION` (esp32: and not a version that already rolled
back, see below), stream the image into the inactive slot while
hashing; verify streamed size + SHA-256 against the manifest; stage + reboot;
after a healthy first poll, **commit** (else the bootloader rolls back). The
mechanism differs per board but the policy above does not:

| board | slots | commit / rollback |
|-------|-------|-------------------|
| pico  | RP2350 A/B partitions | bootrom **TBYB** trial flag; commit clears it |
| esp32 | `HEALTHBAR_C3` `ota_0`/`ota_1` at `0x1D0000` each | `set_boot` + `mark_app_valid_cancel_rollback`, with rollback enabled in the bootloader (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`) |

**esp32 probation:** the OTA check itself runs on the first good poll after
boot, then hourly. Separately, the *image* is confirmed
(`mark_app_valid_cancel_rollback`) on the first server answer of any kind —
a health line or even a 404 unknown-slug counts, since either means the
network and this image are fine. An unconfirmed image that can't reach the
server resets within about 90 s (the offline reset, which fires first) and
at most 180 s (probation), and the bootloader rolls back to the previous
slot. The frozen `boot.py` also resets on an import failure (e.g. a broken
frozen image), so a broken image rolls back the same way instead of dropping
to a REPL.

**esp32 rolled-back versions are retried once, then skipped:** after staging
an update the board adds `"pending": <version>` to `/data/ota.json`. If it
next boots still running a different version, the update rolled back, and
the marker becomes `{"failed": <version>, "attempts": <n>}` (`n` counts up
for the same version and restarts at 1 for a different one; an older
marker without `attempts` counts as 1). The marker is cleared once the new
image is confirmed. A version that rolls back twice is skipped until a
different version is published (any other newer version is offered as
usual), so a bad image isn't re-downloaded every few minutes. The one retry
exists because rollback is triggered by "no server answer": a single
transient outage during probation (a server redeploy longer than 90 s, a
WiFi drop into the setup-mode idle reset, a portal save before the image was
confirmed) would otherwise blacklist a good image. A full flash erase clears
the marker.

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

Fields also settable from the server: see section 4. The esp32 board also
keeps `/data/device.json` (its token + last-applied config rev) alongside
`config.json` — not board hardware config, just the identity section 4 needs.

## 4. Board config channel (esp32)

`GET /device/<mac>/config` — the esp32 board's server-pushed config channel.
`mac` is the board's STA MAC as 12 lowercase hex, no separators. Like
`*.txt` and `/firmware/**`, this path must be routed plain-HTTP and without
Authentik: it's device-to-server, not a user-facing page.

**Request headers:**

| header | meaning |
|--------|---------|
| `X-Device-Token` | 32 lowercase hex, generated by the board on first boot and kept in `/data/device.json` |
| `X-Config-Rev` | the last rev the board applied |
| `X-Firmware-Version` | reported `FIRMWARE_VERSION` |
| `X-Slug` | reported character slug |
| `X-Local-IP` | reported local IP |

**Responses:**

| status | meaning |
|--------|---------|
| `400` | malformed mac or token |
| `403` | token mismatch |
| `429` | server's device limit (64) reached, and this mac is new |
| `304` | board is current (`X-Config-Rev` matches) |
| `200` | JSON payload |

Example `200` body:

```json
{"rev": 7, "slug": "shen", "brightness": 0.4, "poll_seconds": 5,
 "wifi": {"upsert": [{"ssid": "Cabin", "psk": "…", "priority": 2}], "remove": ["OldNet"]}}
```

**Self-registration:** on first contact the server stores `sha256(token)`
(trust on first use) — no pre-provisioning needed. An admin can "forget" a
board; its next check-in re-registers with whatever token it presents.
Forgetting also deletes that board's managed WiFi rows, so their PSKs can't
go to whoever re-registers next.

**Merge rules on the device** (`firmware-esp32/remote_config.py`):
- an absent or null field means "not managed from the server": keep the
  local value
- an invalid field is skipped, but the payload's `rev` is still accepted —
  one bad server value can't cause an endless retry loop
- WiFi is add/update by SSID (`wifi.upsert`), or an explicit `wifi.remove`;
  a remove never touches the currently-connected SSID. A remove of the SSID
  the board is connected to at apply time is ignored and not retried (the
  rev is still accepted); re-save the remove after the board has moved to
  another network (which bumps the rev)
- clamps: `brightness` 0..1, `poll_seconds` 2..300 s; slugs must match
  `[a-z0-9._-]{1,64}` (the setup portal applies the same rule after
  trimming and lower-casing what's typed)

**Never pushable:** `num_leds`, `server_host`/`server_port`, `gpio_pin` — a
bad value there could lock the board out of its own config channel.

**Cadence:** the board checks config right after WiFi connects, before its
first `/<slug>.txt` poll, then every 300 s. A board with WiFi but no slug yet
checks config before falling back to the setup portal, so a fresh board can
be assigned a slug from the admin page without ever entering AP mode. It
does not re-check while in setup mode, but a board with saved WiFi whose
portal has been idle for 180 s reboots, which reconnects and re-checks.
Idle is measured from the later of setup-mode start and the last portal
activity (any POST, or a GET of the form page; OS captive-probe redirects
don't count), so a phone that auto-opens the captive page only delays the
retry. So a slug
assigned while the board is in setup mode is picked up within about 3
minutes, and a board that fell into setup mode during a router outage
recovers by itself. With no saved WiFi, setup mode waits indefinitely.

**Security:** plain HTTP, so WiFi PSKs travel unencrypted on the LAN. The
token only stops other clients from fetching a board's config — it is not
transport security.

## Build / serve / update

One interface, board as the argument (see `justfile`):
`just build <board>`, `just flash <board>`, `just publish <board> <version>`.
`publish` builds the image, stamps the version, and writes a correctly-formatted
manifest into `server/firmware/<board>/` via `server/tools/publish-fw.mjs`.
