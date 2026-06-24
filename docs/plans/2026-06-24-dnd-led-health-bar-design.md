# D&D Beyond LED Health Bar — Software Plan

## Context

Building the software for a physical, glowing LED health bar: a WS2812B strip (16–17 LEDs)
driven by a Raspberry Pi Zero 2 W that mirrors a D&D Beyond character's current HP in
near-real-time. One unit per party member; identical software, per-device config. It must be
reliable (runs unattended in various houses), responsive (LEDs react within seconds of
damage/healing) without getting rate-limited by D&D Beyond, easy to set up by non-technical
players (no-auth web config + bulletproof multi-network WiFi + captive-portal fallback), and
easy to tweak (animations and constants are isolated and hot-reloadable). Inspired by
`powerwordspill/healthbar` (CircuitPython/Pico); we reuse its proven DDB integration approach
but target full Linux on the Pi Zero 2 W.

**Confirmed decisions:** Go · Hybrid poll+websocket sync · build for private-superset but
public-first · guided paste + bookmarklet for credentials.

## Stack & Key Reuse

- **Language:** Go (single static-ish binary, systemd `Restart=always`, goroutines for
  concurrent render loop + DDB poller + websocket + web server).
- **LED driver:** `github.com/rpi-ws281x/rpi-ws281x-go` (cgo wrapper over jgarff `rpi_ws281x`,
  DMA/PWM on GPIO18). Requires the C lib present at build/runtime; runs as root.
- **WiFi:** NetworkManager (Pi OS Bookworm default) driven via `nmcli`; our TOML is the source
  of truth that we sync into NM connection profiles (autoconnect + priority).
- **Web:** Go stdlib `net/http` + `html/template`; tiny vanilla-JS frontend. No external web deps.
- **DDB endpoints (from reference):** `auth-service…/v1/cobalt-token`,
  `wss://game-log-api-live.dndbeyond.com/v1`, `character-service…/character/v5/character/{id}`.

## Architecture

Single Go binary, several goroutines communicating through a shared `HealthState`
(mutex-guarded) and an `events` channel:

```
            ┌─────────────┐   poll (adaptive)    ┌──────────────────┐
            │  DDB poller │ ───────────────────▶ │                  │
            └─────────────┘                      │   HealthState    │
            ┌─────────────┐   push events        │  (cur/max/temp,  │
            │ WS listener │ ───────────────────▶ │   status, deltas)│
            └─────────────┘                      └────────┬─────────┘
            ┌─────────────┐   config/credentials          │ read @ ~30–60fps
            │  web server │ ◀──────────▶ config files      ▼
            └─────────────┘                       ┌──────────────────┐
            ┌─────────────┐   AP fallback          │  Render loop     │──▶ LEDStrip iface
            │  netwatch   │ ◀──────────▶ nmcli      │ (state machine + │   ├─ hw (ws281x)
            └─────────────┘                         │  animations)     │   └─ sim (terminal)
                                                    └──────────────────┘
```

- **LEDStrip interface** with two backends: `hw` (rpi-ws281x-go) and `sim` (ANSI-terminal
  renderer). Lets the whole app `go run` on a laptop with **no hardware** — critical for
  developing/tweaking animations and for CI.
- **Render loop** owns all pixels, runs at a fixed FPS, reads `HealthState`, and drives a small
  **state machine**: `boot → connecting → idle ⇄ damage/heal flash → low-hp → error/offline`.
  Damage/heal transitions are triggered by deltas the poller/WS writes into state.

## Repository Layout

```
cmd/healthbar/main.go         # wiring, flags (--sim, --demo, --config-dir)
internal/config/              # load+watch TOML, hot-reload on SIGHUP / web save
internal/ddb/                 # auth (cobalt→token+refresh), character fetch, HP calc, poller, ws
internal/led/                 # LEDStrip iface, hw backend, sim backend, Color/Frame types
internal/anim/                # discrete animation funcs + state machine + render loop
internal/web/                 # http handlers, templates, bookmarklet, capture endpoint
internal/netcfg/              # wifi.toml ⇄ nmcli sync, captive-portal AP fallback
deploy/                       # healthbar.service, provision.sh, image-prep + soldering docs
testdata/                     # sample DDB character JSON fixtures (public + private)
```

## Config (simple TOML, web-editable, hot-reloaded)

`/etc/healthbar/` (or `/boot/healthbar/` for easy SD-card editing):
- **`device.toml`** — player name, `character_id`, optional `user_id`/`game_id`, `num_leds`,
  `gpio_pin`, base `brightness`, poll intervals.
- **`theme.toml`** — all tunable constants: colors (hi/mid/low/temp-HP/status), HP thresholds,
  brightness, FPS, flash durations, idle-shimmer params, low-HP pulse params. **Read at startup
  and on save** so ideas are easy to try.
- **`wifi.toml`** — list of `{ssid, psk, priority}`; synced to NM. Source of truth.
- **`secrets.toml`** (perms 0600) — Cobalt cookie + cached token. Absent for public-only setups.

## DDB Integration (public-first, private-capable)

1. **Public path (recommended, zero-credential):** if the sheet privacy is "Public", the v5
   character endpoint returns JSON unauthenticated. Poller fetches directly with just
   `character_id`. We'll document the one-toggle "set sheet to Public" as the easy path.
2. **Private path:** POST/GET `cobalt-token` with the pasted Cobalt cookie → `{token, ttl}`;
   refresh before expiry; use `Authorization: Bearer <token>` for fetch + websocket. Auto-detect:
   try public fetch first; on 401/403 fall back to token auth.
3. **HP calc** (`internal/ddb`, table-tested against fixtures): `max = override>0 ? override :
   base + bonus + conMod*totalLevel`; `current = max - removedHitPoints`; `temp =
   temporaryHitPoints` (rendered as overshield). Exact field set validated against a real JSON
   fixture during impl (reference uses base/removed/temp/hitDice + CON).
4. **Adaptive polling:** fast (~2–4s) for a window after any detected change, backing off to
   ~20–30s when idle (jittered) to avoid rate-limiting. Per-device → low aggregate load.
5. **WebSocket (optional, when `game_id` set + token available):** subscribe to game-log events;
   on a relevant character event, immediately trigger a fetch (push-driven responsiveness). WS is
   best-effort; poller remains the reliable baseline. WS unavailable for public-only setups.

## LED Rendering & Animations

- **HP→frame mapping:** fraction → lit-LED count over 16/17; color by threshold gradient
  (green→amber→red); temp-HP shown as distinct color beyond current. All from `theme.toml`.
- **Discrete, individually testable animation funcs** in `internal/anim`, each
  `func(frame, t, params) ` producing a frame so they can be unit-tested (golden frames) and
  previewed via `--demo`:
  - `idle` — subtle breathing brightness + slow shimmer/noise (always-on ambient).
  - `damage` — flash red, then animate bar shrinking to new level.
  - `heal` — flash green, then animate bar growing to new level.
  - `lowHP` — pulse/heartbeat red below threshold.
  - `status` — boot, connecting, offline/error patterns (distinct, so failures are visible).
- **Hot-reload:** saving `theme.toml` (web or file) re-reads constants live.

## Web Config UI (no-auth, LAN/USB/AP reachable)

- Status page: current HP, connection state, last update, which WiFi/IP.
- Editors for `device.toml`, `theme.toml`, `wifi.toml` (add/remove networks with scan list).
- **Credential capture:** guided, honest flow:
  - **IDs via bookmarklet:** a served bookmarklet, run on an open DDB tab, scrapes
    `character_id`/`user_id`/`game_id` from URL/DOM and POSTs them to the bar — removes the
    most error-prone step.
  - **Cookie via guided paste:** because `CobaltSession` is HttpOnly (not JS-readable), the UI
    gives copy-paste-from-devtools steps for the cookie. (Skipped entirely for public sheets.)
  - The original "iframe DDB to auto-grab everything" idea is **rejected**: DDB blocks framing
    (X-Frame-Options) and browsers isolate cross-origin + HttpOnly cookies.
- Binds on all interfaces (LAN, `usb0`, AP) on a fixed port; mDNS name (e.g. `healthbar.local`).

## WiFi (multi-network) & Captive Portal

- On boot, `netcfg` syncs `wifi.toml` → NM profiles (autoconnect, priority) so the unit joins
  whichever known house network is present automatically.
- **Captive-portal fallback:** if no connectivity after a timeout, `netwatch` starts NM AP mode
  (`healthbar-setup` SSID) + `dnsmasq` wildcard DNS + portal-detection 302s → serves the same web
  UI's WiFi-add form. On successful join, AP drops. Bulletproof first-run and venue changes.

## USB-C SSH (gadget mode) & Provisioning

- One-time **image prep** (documented + scripted in `deploy/`): `dtoverlay=dwc2` in `config.txt`,
  `modules-load=dwc2,g_ether` in `cmdline.txt`, static `usb0` (e.g. 10.55.0.1) + mDNS, SSH on.
  Soldering note: power + D+/D- to the Pi Zero's **data** USB port.
- **Cross-compile** for arm (Pi Zero 2 W is armv7/arm64) via the rpi-ws281x-go docker toolchain;
  ship binary + config templates + `healthbar.service` (root, restart-always). `provision.sh`
  installs the C lib, places files, enables the service. Per-device: flash same image, set
  `device.toml` (player + character).

## Build Phases (milestones)

1. **Skeleton + sim:** config load/watch, LEDStrip iface, terminal sim, static HP render —
   runnable on a laptop.
2. **Animations + state machine + render loop** (all sim-tested, `--demo` cycles them).
3. **DDB client:** public-first fetch, HP calc (fixtures), adaptive poller → drives state.
4. **Auth + websocket:** cobalt-token + refresh, WS subscribe → push fetch.
5. **Web UI:** status + config editors + bookmarklet/paste capture.
6. **WiFi sync + captive portal.**
7. **Hardware bring-up:** ws281x backend on Pi, systemd, USB gadget, `provision.sh`.
8. **Polish + per-device imaging guide.**

## Testing & Verification

- **Unit (laptop, no hw):** HP calc (table-driven over JSON fixtures incl. temp/override/edge
  cases), fraction→frame mapping, each animation (golden frames), config parse + hot-reload,
  `wifi.toml`→nmcli command generation.
- **Simulator E2E:** `go run ./cmd/healthbar --sim --demo` to eyeball animations; `--sim` against
  a live **public** test character to watch real HP drive the terminal bar.
- **DDB client:** unit tests against recorded fixtures; one live public-character smoke test.
- **On-device:** flash a Pi, run service, confirm strip lights + reacts; verify WiFi
  multi-network failover, AP captive portal on unknown network, and SSH-over-USB.
- **Acceptance:** take damage on DDB → bar flashes red and shrinks within a few seconds; heal →
  green grow; lose WiFi → status pattern; unknown venue → AP portal lets you add the network.

## Notes / Risks

- DDB is unofficial/undocumented; endpoints can change (reference disclaimer). Isolate all DDB
  specifics in `internal/ddb` behind an interface for easy repair.
- cgo + cross-compile is the main build friction; docker toolchain mitigates. Falls back to
  building on-Pi if needed.
- Exact v5 HP field semantics confirmed against a real fixture early in Phase 3.
