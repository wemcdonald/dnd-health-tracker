# Health Bar firmware — Raspberry Pi Pico 2 W (MicroPython)

A D&D Beyond–driven LED health bar. It polls a **public** character's hit points
and renders them on a WS2812B strip (green→amber→red gradient with damage/heal
flashes and a low-HP heartbeat). Runs bare-metal on a Pico 2 W in MicroPython.

> Rewritten from the original Go/Raspberry-Pi-Zero-2-W implementation, archived
> in [`../legacy-go/`](../legacy-go/). See
> [`../docs/plans/2026-06-24-pico2w-healthbar-design.md`](../docs/plans/2026-06-24-pico2w-healthbar-design.md).

## Modules

| File | Role |
|------|------|
| `boot.py` / `main.py` | Boot, mode selection, core-1 render thread, WDT, recovery |
| `config.py` / `colors.py` | JSON config (device/theme/wifi) + RGB color helpers |
| `hp.py` | Streaming D&D Beyond HP extractor (never buffers the full sheet) |
| `ddb.py` | HTTPS fetch (socket+ssl, chunked-aware) + adaptive poller |
| `anim.py` / `leds.py` | Animation state machine + WS2812B / terminal-sim backends |
| `wifi.py` / `portal.py` | Multi-network STA connect; AP captive portal + setup UI |
| `data/*.json` | Default config installed on first push |

## Two modes

- **RUN** — connected to a known network *and* a character is configured: runs
  the TLS poll loop + LED render. No web server resident.
- **SETUP** — otherwise (no known WiFi reachable, or no character set): brings up
  the `healthbar-setup` access point with a captive portal at `192.168.4.1` to
  enter WiFi + character ID. On save it reboots and connects automatically.

They are mutually exclusive so only one memory-heavy subsystem (TLS *or* HTTP
server) is ever resident — the main RAM lever on the 520 KB device.

## Flashing & deploying

1. **MicroPython firmware (once):** download the Pico 2 W (RP2350, with cyw43
   WiFi) MicroPython `.uf2`, hold BOOTSEL while plugging in USB, and drop the
   `.uf2` onto the `RPI-RP2` drive.
2. **Push the app:** `pip install mpremote`, then:
   ```sh
   ./push.sh           # copies modules + default config, resets the board
   ./push.sh --run     # …and opens the REPL to watch logs
   ```
   `push.sh` keeps any existing `data/*.json` on the device so your config and
   saved WiFi survive re-deploys.

## Wiring

- WS2812B data → the GPIO in `data/config.json` (`gpio_pin`, default 18), through
  a level shifter (e.g. 74AHCT125): Pico GPIO is 3.3 V, WS2812B wants ~5 V data.
- LED 5 V from VBUS or an external 5 V supply; **common ground** with the Pico.

## Development & tests

All logic modules are plain Python and run under CPython, so the test suite needs
no hardware:

```sh
cd firmware
for t in tests/test_*.py; do python3 "$t"; done   # unit tests
python3 demo.py                                    # live ANSI-sim preview
```

`demo.py` plays boot → connect → scripted damage/heal/low-HP/temp so the
animations can be eyeballed in a terminal.
