# D&D Beyond LED Health Bar — Pico 2 W Design

## Context

A D&D Beyond–driven LED health bar: it polls a character's hit points and renders
them on a WS2812B strip (green→amber→red gradient with damage/heal flashes and a
low-HP heartbeat). The original implementation (archived in `legacy-go/`) was
3,828 LOC of Go targeting a **Raspberry Pi Zero 2 W** — a full Linux SBC, with
systemd, NetworkManager, dnsmasq, a filesystem, and a cgo WS2812B driver.

The hardware target changed to a **Raspberry Pi Pico 2 W**: an RP2350
microcontroller (dual Cortex-M33 @150 MHz, ~520 KB SRAM, ~4 MB flash, WiFi via
CYW43439, **no OS**). None of the Linux machinery exists here, so this is a
ground-up rewrite in **MicroPython**, not a port. The hardware-agnostic *logic*
(HP formula, animation curves, adaptive poll cadence, config schema) was carried
over faithfully from the Go code; everything touching the OS, network stack, and
GPIO was rebuilt.

**Decisions (locked with the user):** MicroPython runtime · poll-only over HTTPS
(drop the websocket) · setup-mode captive portal that the device also falls into
automatically whenever no known WiFi connects · public characters only (no
Cobalt-token auth).

**RAM is the binding constraint.** A single mbedtls TLS handshake costs ~40–60 KB.
The design keeps **only one memory-heavy subsystem resident at a time**.

## Architecture

Two mutually-exclusive modes, chosen by the supervisor at boot
(`main.choose_mode`):

- **RUN** — STA connected to a known network *and* a character configured: the
  HTTPS poll loop + LED render. No HTTP server resident.
- **SETUP** — otherwise: AP `healthbar-setup` + captive portal + minimal web UI.
  No TLS client resident. On save, writes config to flash and reboots into RUN.

**Concurrency.** A `uasyncio` loop on core 0 runs the poller / supervisor (or the
portal in SETUP). The **LED render loop runs on core 1 via `_thread`** so
animations stay smooth while a blocking TLS fetch occupies core 0. The shared
`anim.Engine` state is guarded by a lock.

**Reliability** (replacing systemd `Restart=always`): the core-1 render thread
feeds a hardware `WDT`; `run()` is wrapped so any unhandled exception triggers
`machine.reset()`; sustained WiFi loss resets the board, which re-runs network
selection and drops into the portal if nothing connects.

### Modules (and the Go package each replaces)

| Module | Replaces | Responsibility |
|--------|----------|----------------|
| `main.py` / `boot.py` | `cmd/healthbar`, `internal/app` | Boot, mode selection, render thread, WDT, recovery |
| `config.py` / `colors.py` | `internal/config`, `internal/led` (Color) | JSON config + atomic save; RGB helpers |
| `hp.py` | `internal/ddb/hp.go` + `types.go` | Streaming HP extractor + v5 formula |
| `ddb.py` | `internal/ddb` (client/poller) | HTTPS socket+ssl fetch (chunked-aware) + steady poller (nudgeable) |
| `anim.py` | `internal/anim` | Gradient/effects + state machine |
| `leds.py` | `internal/led` | `neopixel` (PIO) + ANSI-sim backends |
| `wifi.py` | `internal/netcfg` (manager/watch) | Multi-network STA + AP |
| `portal.py` | `internal/netcfg` (portal) + `internal/web` | Wildcard DNS + setup HTTP UI |

### Data flow

```
boot → load config.json/theme.json/wifi.json from /data (littlefs)
     → start core-1 render thread (reads anim.Engine)
     → wifi.connect_known()  ── ok + character set ──► RUN: ddb.Poller.run()
                             └─ otherwise ───────────► SETUP: AP + portal.serve()
                                                       on save → machine.reset()
```

## Key engineering decisions

1. **Streaming HP extraction (highest risk, solved).** The v5 character sheet is
   ~50–100 KB and cannot be buffered alongside TLS. `hp.py` runs an incremental,
   byte-at-a-time JSON SAX scanner that keeps only a small path stack plus the
   handful of scalar fields the formula needs (HP totals, CON from `stats`/
   bonus/override, `classes[].level`, and `constitution-score` modifiers). Long
   strings it doesn't need are scanned past without being stored, so peak memory
   is flat regardless of document size. An `overrideHitPoints` shortcut lets it
   stop early. Validated against a fixture and a full-parse oracle, including
   byte-by-byte chunk feeding (`tests/test_hp.py`).
2. **TLS verification.** Uses the platform default (no cert verification on
   MicroPython) — read-only public HP data, low MITM stakes. Deliberate tradeoff.
3. **JSON not TOML.** MicroPython has no TOML parser; config moved to JSON using
   the built-in `json` module. Atomic writes (temp + `os.rename`).
4. **HTTP body decoding.** `ddb._Response` streams both `Content-Length` and
   `Transfer-Encoding: chunked` bodies straight into the scanner, forcing
   `Accept-Encoding: identity` (the device can't gunzip).
5. **Steady poll, no idle backoff.** HP events can be minutes apart even
   mid-combat, so an idle tier would make the *first* hit after a quiet stretch
   always slow. The poller runs one steady interval (~5 s, ±15% jitter). The
   optional websocket push can `nudge()` it for instant fetches, letting the
   interval relax to a slow safety-net when a live session is connected.

## RAM budget for the optional websocket (verified)

We dropped WSS in the first cut to avoid a *second* concurrent TLS session. The
numbers say it actually fits, so it's added back as an opt-in (gated on a
configured `game_id`/`user_id`/cookie). MicroPython's mbedtls config
(`extmod/mbedtls/mbedtls_config_common.h`) sets the record buffers to
**`IN_CONTENT_LEN` = 16 KB** and **`OUT_CONTENT_LEN` = 4 KB**, so:

- Each established TLS session ≈ **~24–30 KB** resident (20 KB record buffers +
  context). Handshake adds a transient **~30–50 KB** peak (ECC/bignum scratch),
  freed once connected. We use `CERT_NONE`, so there is **no CA bundle resident
  and no chain verification** — that directly trims the handshake peak.
- The WSS socket is persistent but mostly idle (~28 KB). The poll connection is
  intermittent (`Connection: close`). They are different hosts, so peak overlap
  = WSS resident (~28 KB) + one poll handshake (~45 KB) ≈ **~75 KB**, settling to
  ~28 KB between polls.
- Pico 2 W free heap is ≈ **~400 KB** (RP2350 has 520 KB SRAM; conservatively
  ≥350 KB free after firmware + cyw43). So ~75 KB peak is **~20 % of heap — ~5×
  headroom.**

Conclusion: dual-TLS fits. The real risk is long-run heap **fragmentation** from
mbedtls alloc/free churn, not absolute size. Mitigations: `gc.collect()` before
each poll handshake; never buffer WSS frames; and a **graceful-degradation
guard** — if the WSS task hits `MemoryError` (or any repeated failure), it marks
itself down and the poller automatically tightens from the slow safety-net
interval (~30 s) back to the responsive ~5 s, so the bar keeps working. WSS is
strictly a latency *bonus*, never a dependency. To be confirmed on hardware by
logging `gc.mem_free()` across a soak with both connections live.

## Dropped vs. the Go design

- Go → MicroPython · systemd → `main.py` + WDT + reset-on-fault
- NetworkManager/`nmcli`/`dnsmasq` → cyw43 STA/AP + hand-rolled DNS portal
- USB-gadget SSH → UF2 firmware + `mpremote` push + AP portal
- WSS push + Cobalt auth → **opt-in** (gated on `game_id`/`user_id`/cookie); the
  default remains poll-only + public characters
- TOML → JSON · always-on web UI → setup-mode-only, freed at runtime
- `rpi-ws281x` cgo (root) → `neopixel`/PIO · full-JSON parse → streaming extractor

## Verification

- **Desktop (CPython):** `tests/test_*.py` cover the HP scanner (vs. oracle +
  chunk-boundary stress), animation engine, LED sim, HTTP chunked/Content-Length
  decode + 403, poll cadence + error backoff, WiFi selection/priority, the DNS
  builder, form parsing, the save handler, and mode selection. `demo.py` previews
  the animations in a terminal. All green.
- **On device (manual):** flash MicroPython UF2 → `./push.sh` → power on with no
  saved WiFi → confirm `healthbar-setup` AP + captive portal → enter WiFi +
  public character ID → board reboots, connects, and the strip tracks live HP;
  change HP on the sheet and confirm the bar animates within the fast-poll
  window; drop WiFi and confirm it recovers; soak ≥1 h watching `gc.mem_free()`.

## Open items

- Capture a **real** DDB v5 sheet to confirm field names/shape match the fixture.
- Confirm GPIO pin + LED count against the physical build (defaults: GPIO 18, 16).
- Optional: CA pinning for TLS; on-STA config page (vs. AP-only) for reconfig.
