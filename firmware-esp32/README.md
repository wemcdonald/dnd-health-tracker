# firmware-esp32 — D&D Health Bar (ESP32, MicroPython)

ESP32 port of the health-bar firmware, targeting the **Seeed XIAO ESP32-C3**
(USB-C + onboard LiPo charging). It is a **thin poller**: the server does all the
D&D Beyond work and publishes a precomputed line per character; this firmware
just fetches it over plain HTTP and drives a WS2812B strip, with self-update OTA.

Power is handled in hardware, not firmware: the bar runs off a standalone LiPo
charger board whose switched **output** feeds the device, so a plain power switch
gives true-off (zero draw) while USB still charges the cell upstream. No on-device
sleep logic. See `../docs/hardware-build.md`.

The Pico 2 W builds (`../firmware`, `../firmware-c`) remain the baseline; this
tree is the forward path once ESP32 hardware is in hand.

## Why MicroPython

The device logic is tiny and the existing Pico firmware is already MicroPython,
so most of it (`anim.py`, `colors.py`, `leds.py`, `wifi.py`, `portal.py`,
`config.py`) ports with little or no change. ESP32 gives native WiFi (no cyw43
cold-load hang), `neopixel` over RMT, and built-in dual-app OTA with rollback.

## Files

| File | Role |
|------|------|
| `boot.py` | entry; calls `main.run()` |
| `main.py` | supervisor: connect → poll loop → sleep → OTA; render thread; recovery |
| `poll.py` | plain-HTTP GET of `/<slug>.txt` + 4-int wire parser |
| `ota.py` | manifest parse + stream image into next OTA partition + rollback |
| `device.py` | board identity: MAC id + self-generated token (`/data/device.json`) |
| `remote_config.py` | fetches + merges server-pushed config (slug/brightness/poll_seconds/WiFi) |
| `wifi.py` | `network.WLAN` STA/AP manager |
| `portal.py` | captive portal + config form (WiFi + server/slug) |
| `config.py` | JSON config/theme/wifi in flash |
| `anim.py`, `colors.py`, `leds.py` | LED animation engine + WS2812/sim backends (reused) |
| `version.py` | `FIRMWARE_VERSION` (compared against the OTA manifest) |
| `data/config.json` | default device config |
| `board/HEALTHBAR_C3/` | custom MicroPython board: dual-OTA partitions, sdkconfig, frozen-manifest hookup |
| `tests/` | CPython host tests (`python3 tests/test_*.py`) |

## Pins & wiring

Chosen pin (you can override via `gpio_pin` in `data/config.json`):

| Signal | XIAO C3 pad | GPIO (`gpio_pin`) | Why |
|--------|-------------|------|-----|
| **WS2812B data** | **D1** | **GPIO3** | Clean pin: not a strapping/boot pin (avoids GPIO2/8/9 = D0/D8/D9), not UART0 (avoids GPIO20/21 = D6/D7), no default I²C/SPI bus. `config.json` `gpio_pin` uses the **GPIO number (3)**, not the pad label. |

Power the XIAO at its underside **BAT+ / BAT−** pads (battery input, 3.7–4.2 V) —
not the 5V/3V3 pins. Common ground across XIAO, strip, and the charger's `OUT−`.

- WS2812B data is 5 V logic; the ESP32 drives 3.3 V. Use a level shifter
  (e.g. 74AHCT125) **or** power the strip at ~4.5 V so its logic-high threshold
  drops. Same as the Pico build.
- **Power the strip from the battery (BAT), not the 3V3 pin** — the XIAO's 3V3
  regulator is 500 mA and 16 LEDs at full white approach ~1 A. Keep brightness
  modest on battery. Common ground with the ESP32.
- **Power switch:** inline SPST on the LiPo `+` lead (cell → switch → BAT+ pad),
  rated ≥2 A. See `../docs/hardware-build.md`.

## Power (hardware, not firmware)

The bar has no on-device sleep. Instead it runs off a standalone LiPo charger
board (e.g. a protected TP4056) with **separate battery and output** terminals:

```
LiPo ──(B+/B-)── charger board ── USB-C in (charges the cell anytime)
                     OUT+ ──[ power switch ]── XIAO BAT+ pad  &  strip +
                     OUT- ───────────────────  common GND
```

- **Switch OFF** cuts the whole device (MCU + strip) → **true zero draw**.
- The charger sits upstream on `B+/B-`, so USB **still charges the cell while the
  device is switched off**.
- Charge only via the charger board's USB-C; leave the XIAO's own USB unused for
  power so its onboard charger stays dormant.

This is why the firmware carries no deep-sleep/strip-gating: the switch does the
job in hardware, without the WS2812's ~10–16 mA idle draw that MCU sleep can't
touch.

## OTA

Consumes the server's existing endpoints (plain HTTP, no auth):
`GET /firmware/esp32/latest` (manifest `"<version> <size>\n<sha256>\n<imagePath>"`) and
`GET /firmware/esp32/image.bin`.

Flow (`ota.py` + `main.py`): while online, on the first good poll after boot
and then at most every `OTA_CHECK_EVERY_S` (1 h), fetch the manifest; if its
`version` > `version.FIRMWARE_VERSION`, stream `image.bin` straight into the
*next* OTA partition (never buffered whole), verify size + SHA-256,
`set_boot`, and reboot. Separately, the image is **confirmed**
(`Partition.mark_app_valid_cancel_rollback()`) on the first server answer of
any kind after boot — a health line or even a 404 unknown-slug counts. An
image that can't reach the server resets within about 90 s (the offline
reset) and at most `PROBATION_S` (180 s), and the bootloader rolls back to
the previous slot; a broken frozen image (import failure in `boot.py`) rolls
back the same way. A version that rolls back is retried once, then skipped
until a different version is published: `/data/ota.json` gains
`"pending": N` after staging, and becomes `{"failed": N, "attempts": n}` if
the board next boots on another version (see `../docs/firmware-contract.md`
section 2). A full flash erase clears it.
Partitions
(`board/HEALTHBAR_C3/partitions.csv`): `ota_0`/`ota_1` at `0x1D0000` (1856
KiB) each, plus a 320 KiB `vfs` for `/data`, with
`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` in `sdkconfig.board`.

## Toolchain setup

- **ESP-IDF v5.5.2** at `~/esp/esp-idf` — `./install.sh esp32c3`, then
  `source ~/esp/esp-idf/export.sh` in every shell you build from. If that
  sourcing throws Python errors, an asdf free-threaded `python3` shim breaks
  the IDF env — strip `~/.asdf/shims` from `PATH` first.
- **MicroPython v1.29.0** checked out at `../micropython` (or set
  `MICROPYTHON_DIR`) — build `mpy-cross` and fetch the port's submodules
  (`make ... submodules`) once before the first `just build esp32`.

## Build

```sh
just build esp32 <version>      # <version> must be an integer
```

Drives `idf.py` plus `makeimg.py` directly rather than `make BUILD=...`: a
`BUILD=` on the make command line leaks (via `MAKEFLAGS`) into the mpy-cross
sub-make and corrupts its generated headers. The build enforces >= 128 KiB
free in the 1856 KiB OTA slot. After editing `sdkconfig.board` or
`partitions.csv`, `rm -rf firmware-esp32/build/idf` first — ESP-IDF only
fills in `SDKCONFIG_DEFAULTS` keys missing from an existing
`build/idf/sdkconfig`; it never overwrites ones already set there.

## First flash

```sh
ESP32_PORT=/dev/cu.usbmodemXXXX just flash-full esp32
```

**Erases the whole board, including `/data`** — WiFi and the character slug
must be re-entered via the `healthbar-setup` captive portal afterwards.

First boot with no WiFi brings up the `healthbar-setup` AP (password
`dndhealthbar`); connect and the captive portal opens — enter WiFi + the
character **slug** (and server host if not the default). The bar reboots and
starts polling. The slug is trimmed and lower-cased, and must be
`[a-z0-9._-]{1,64}`; anything else is rejected with a message and nothing is
saved. Leave the slug blank to have it assigned from the admin page instead.

Setup mode with WiFi already saved (no slug yet, or WiFi was unreachable at
boot) reboots once the portal has been idle for 180 s (no page load or save),
to reconnect and pick up a slug assigned from the admin page in the
meantime. Each page load or save restarts that timer.

## Publish OTA

```sh
just publish esp32 <version>
```

Builds the image, stamps the version, and writes the manifest into
`server/firmware/esp32/` via the same `publish-fw.mjs` tool the Pico uses.

## Dev flow

`just flash esp32` (mpremote copy) is for a **stock MicroPython** build only:
on the OTA (frozen) build, `boot.py` puts the frozen app ahead of filesystem
copies, so files copied this way are ignored. mpremote needs `resume` in the
session, because `boot.py` runs the app and never returns.

## Remote config

The board also checks in with the server for pushed config (slug,
brightness, poll_seconds, WiFi) — see `../docs/firmware-contract.md` section
4 and `remote_config.py` / `device.py`.

## Tests

```sh
cd firmware-esp32
for t in tests/test_*.py; do python3 "$t"; done
```

Host tests cover the pure logic: wire parse (`poll`), OTA manifest parse and
rollback-skip marker (`ota`), setup-mode idle reset (`main`), the portal's
save handler (`portal`), remote config (`remote_config`, `device`), config
URL, and the animation engine. **Not** host-testable (needs the board):
WiFi join, neopixel timing, and OTA partition writes/rollback.
