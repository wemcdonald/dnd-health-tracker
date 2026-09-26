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
| `wifi.py` | `network.WLAN` STA/AP manager |
| `portal.py` | captive portal + config form (WiFi + server/slug) |
| `config.py` | JSON config/theme/wifi in flash |
| `anim.py`, `colors.py`, `leds.py` | LED animation engine + WS2812/sim backends (reused) |
| `version.py` | `FIRMWARE_VERSION` (compared against the OTA manifest) |
| `data/config.json` | default device config |
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
`GET /firmware/latest` (manifest `"<version> <size>\n<sha256>\n<imagePath>"`) and
`GET /firmware/image.bin`.

Flow (`ota.py`): while online, at most every `OTA_CHECK_EVERY_S` (1 h), fetch the
manifest; if its `version` > `version.FIRMWARE_VERSION`, stream `image.bin`
straight into the *next* OTA partition (never buffered whole), verify size +
SHA-256, `set_boot`, and reboot. The first good poll after boot calls
`Partition.mark_app_valid_cancel_rollback()`, so an image that can't connect/poll
**rolls back** on the next reset.

**Build requirements for OTA:**
- Flash a MicroPython build with the standard **dual OTA app partition table**
  (ota_0 / ota_1 + otadata), not a single-app layout.
- Enable rollback in the build (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`) for
  the auto-revert to work.
- The published `image.bin` is a full MicroPython **application image** with this
  firmware frozen in (or the app copied to the filesystem, depending on how you
  package). Bump `version.py` and the manifest `version` in lockstep on each
  publish. Publish via the existing `server/tools/publish-fw.mjs` into the
  server's `/firmware` dir.

## Flashing (first time)

```sh
# 1. Erase + flash MicroPython for ESP32-C3 (dual-OTA build; see OTA above)
esptool.py --chip esp32c3 erase_flash
esptool.py --chip esp32c3 write_flash -z 0x0 ESP32_GENERIC_C3-OTA.bin

# 2. Copy the app + config (mpremote)
cd firmware-esp32
mpremote fs mkdir :/data 2>/dev/null || true
mpremote fs cp data/config.json :/data/config.json
for f in *.py; do mpremote fs cp "$f" ":/$f"; done
mpremote reset
```

First boot with no WiFi brings up the `healthbar-setup` AP (password
`dndhealthbar`); connect and the captive portal opens — enter WiFi + the
character **slug** (and server host if not the default). The bar reboots and
starts polling.

## Tests

```sh
cd firmware-esp32
for t in tests/test_*.py; do python3 "$t"; done
```

Host tests cover the pure logic: wire parse (`poll`), OTA manifest parse (`ota`),
config URL, and the animation engine. **Not** host-testable (needs the board):
WiFi join, neopixel timing, and OTA partition writes/rollback.
