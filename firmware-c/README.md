# D&D Health Tracker — firmware (C / Pico SDK)

C rewrite of the MicroPython firmware for the Raspberry Pi Pico 2 W (RP2350).
The device is deliberately dumb: it joins WiFi, polls `http://<server>/dnd/<slug>.txt`
over **plain HTTP** every ~2s, parses one line (`<lit> <age_s>`), and lights
`lit` of 16 WS2812 LEDs. All the hard work (TLS, HP math, D&D Beyond WSS) lives
in `../server`.

## Toolchain (macOS)

```bash
brew install cmake
brew install --cask gcc-arm-embedded   # full Arm toolchain WITH newlib (run in a real terminal; sudo .pkg)
# Pico SDK + examples (submodules matter):
git clone --recurse-submodules https://github.com/raspberrypi/pico-sdk ~/code/pico-sdk
git clone https://github.com/raspberrypi/pico-examples ~/code/pico-examples
export PICO_SDK_PATH=~/code/pico-sdk
```

> The Homebrew `arm-none-eabi-gcc` *formula* is compiler-only and will fail to link
> (`cannot read spec file 'nosys.specs'`). Use the `gcc-arm-embedded` **cask**.

## Build

```bash
cmake -B build -DPICO_BOARD=pico2_w -DHEALTHBAR_NAME=shen
cmake --build build -j4
```

Outputs `build/m0_blink.uf2` (sanity) and `build/m1_portal.uf2` (the real app).

### Build options
| option | default | meaning |
|--------|---------|---------|
| `HEALTHBAR_NAME` | _(empty)_ | setup-AP SSID becomes `healthbar-setup-<name>`; also the default character slug in the portal form |
| `POLL_HOST` | `public.willflix.com` | server host the device polls |
| `POLL_PORT` | `80` | server port |
| `DEV_SEED_CONFIG` | `OFF` | **dev only** — seed wifi config on first boot from `DEV_SEED_SSID/PSK/SLUG` (provision without a phone). Never use for a real build. |

### Production build (normal)
```bash
cmake -B build -DPICO_BOARD=pico2_w -DHEALTHBAR_NAME=shen \
      -DPOLL_HOST=public.willflix.com -DPOLL_PORT=80
cmake --build build -j4
```
First boot has no config → raises the `healthbar-setup-shen` open AP → connect a
phone → captive portal pops → enter WiFi + slug → saved to flash → reboots and
connects.

## Flashing

- **BOOTSEL**: hold BOOTSEL while plugging USB, drag the `.uf2` onto the `RP2350` drive.
- **picotool** (no button): `picotool load build/m1_portal.uf2 -f -x`
- **Serial**: USB-CDC at `/dev/cu.usbmodem*` (also UART via picoprobe, independent of USB).

## Architecture

- `pico_cyw43_arch_lwip_threadsafe_background`: all cyw43/lwIP/flash on **core0**;
  **core1** only renders LEDs from a spinlock-guarded `health_t`.
- **Cold-boot-hang fix**: the radio plays exactly one role per boot — STA (connect)
  *or* AP (portal), split by a reboot — so there is never a scan-while-AP on a cold
  cyw43 (the MicroPython hang). A failed STA connect sets a watchdog-scratch flag and
  reboots into the portal (no retry loop).
- `config.c`: wifi nets + slug persisted in the last flash sector (magic + CRC32),
  written only with core1 locked out.
- `http_poll.c`: lwIP raw-TCP plain-HTTP GET, reconnect per poll, `sscanf("%d %d")`.
- `leds.c`: WS2812 on GPIO18 (16 LEDs), 30 fps; gradient by `lit/16`, low-HP
  heartbeat, damage/heal flash, boot sweep, connecting dot, offline breathing.

## Milestone status (verified on hardware unless noted)

| | milestone | status |
|--|-----------|--------|
| M0 | blink + toolchain | ✅ |
| M1 | captive portal (open AP, form, URL-decode, parse) | ✅ |
| M2 | flash persistence (survives reboot) | ✅ |
| M3 | STA connect to real WiFi (cold-boot fix) | ✅ |
| M4 | HTTP poll — full device→server→DDB loop + offline/recovery | ✅ |
| M5 | LED render engine | ⏳ builds + runs (no regression to M1–M4); **LED visuals unverified — no strip wired yet** |

To verify M5: wire a 16-LED WS2812 strip to **GPIO18** (+5V, GND), flash, and
watch the bar track HP (full = green, low = red + heartbeat, damage = red flash,
server down = breathing).
