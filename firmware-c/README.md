# D&D Health Tracker — firmware (C / Pico SDK)

C rewrite of the MicroPython firmware for the Raspberry Pi Pico 2 W (RP2350).
The device is deliberately dumb: it joins WiFi, polls `http://<server>/dnd/<slug>.txt`
over **plain HTTP** every ~2s, parses one line (`<lit> <age_s>`), and lights
`lit` of 16 WS2812 LEDs. All the hard work (TLS, HP math, D&D Beyond WSS) lives
in `../server`.

## Quick start (`just`)

The firmware is **generic** — identity (character slug + WiFi) is provisioned at
runtime, so you build once and provision per device.

```bash
just build                                   # build the generic firmware
just deploy                                  # flash over USB (no BOOTSEL)
just set name shen                           # character slug -> healthbar-shen.local, /dnd/shen.txt
just set wifi MySSID 'pass' [SSID2 'pass2']  # WiFi networks (priority = order)
just show                                    # print current on-device config
```

Provisioning works two ways (both persist to flash):
- **Captive portal** (most reliable): on a device with no config, join the open
  `healthbar-setup` AP, fill the web form. **Recommended.**
- **USB serial** (`just set ...`): convenient, no AP join — but USB-CDC serial can
  be flaky on some hosts; if a `set` seems not to take, re-run or use the portal,
  and confirm via the status page (below).

**Observability:** the status page at `http://healthbar-<slug>.local/` shows live
state, lit/16, upstream age, IP, and uptime — use it instead of serial (the cyw43
build's USB-CDC *output* is unreliable on some hosts; the device still works).

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
| `ENABLE_STATUSD` | `ON` | STA-mode status web server + mDNS (`healthbar-<slug>.local`). `-DENABLE_STATUSD=OFF` for a minimal build. |
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
- `statusd.c`: STA-mode status web page (`http://healthbar-<slug>.local/` or the
  device IP) showing state/lit/age/IP/uptime + a "Reconfigure WiFi" button (reboots
  into the setup portal via a watchdog-scratch flag). mDNS via lwIP's responder.
- **Hardware watchdog** (~8s) enabled after cyw43 init, fed by **core0** loops only
  (never core1 — that would mask a core0 hang). The wifi connect uses the async API
  so it feeds while joining. NOTE: it does not false-trip, but recovery-from-hang is
  unverified on RP2350 (it did not fire during one deliberate hang — the RP2350
  watchdog tick setup may need attention). The real anti-brick fix is that
  `config_save` no longer deadlocks; the watchdog is a best-effort net.
- **USB-serial provisioning** (`provision.c`): `name`/`wifi`/`show`/`reboot` line
  commands on USB-CDC; `name`/`wifi` save to flash and reboot. `config_save` stops
  core1 (reset) before the flash write — every save is followed by a reboot — which
  avoids the multicore-lockout deadlock that previously froze the board and corrupted
  the config sector.

## Milestone status (verified on hardware unless noted)

| | milestone | status |
|--|-----------|--------|
| M0 | blink + toolchain | ✅ |
| M1 | captive portal (open AP, form, URL-decode, parse) | ✅ |
| M2 | flash persistence (survives reboot) | ✅ |
| M3 | STA connect to real WiFi (cold-boot fix) | ✅ |
| M4 | HTTP poll — full device→server→DDB loop + offline/recovery | ✅ |
| M5 | LED render engine | ⏳ builds + runs (no regression to M1–M4); **LED visuals unverified — no strip wired yet** |
| — | status server + mDNS (`healthbar-<slug>.local`) | ✅ verified: page serves live state, mDNS resolves |
| — | provisioning (captive portal + USB `just set`) | ✅ portal verified end-to-end (persist→connect→mDNS); USB serial works but flaky on some hosts |
| — | hardware watchdog | ⚠️ enabled, no false-trip; recovery-from-hang unverified on RP2350 |

To verify M5: wire a 16-LED WS2812 strip to **GPIO18** (+5V, GND), flash, and
watch the bar track HP (full = green, low = red + heartbeat, damage = red flash,
server down = breathing).
