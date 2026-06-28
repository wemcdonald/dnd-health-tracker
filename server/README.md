# D&D Health Tracker — server

Does all the heavy lifting the Pico 2 W bars can't: TLS to D&D Beyond, HP math,
the game-log websocket (with polling fallback), and Cobalt auth. Publishes one
tiny precomputed file per character that the devices poll over **plain HTTP**.

## What the device fetches

`GET http://dndhealth.willflix.org/<slug>.txt` → the device parses **line 1 only**:

```
8 2
HP 23/45 (+5 temp) · 51%
```

- Line 1: `<lit> <age_s>` — `lit` (0–16) = LEDs to light (precomputed here); `age_s` =
  seconds since the server last refreshed from D&D Beyond.
- Line 2+: human-readable, for browser spot-checking; ignored by the device.
- Device parse is literally `sscanf(body, "%d %d", &lit, &age_s)`.

## Run locally

```bash
cd server
npm install
npm run dev          # http://localhost:8080
```

Open `http://localhost:8080`, add a character (paste its D&D Beyond URL or id, pick
a slug), then:

```bash
curl http://localhost:8080/<slug>.txt
```

You should see the `lit age` line within a few seconds. Change HP on the real
sheet → the file updates (≤5s via polling; instantly once WSS is configured).

## How it works

- **Polling** (`character-service.dndbeyond.com`) — **public** characters need no
  auth. For a **private** sheet, set a **Cobalt cookie** in the admin UI: the
  server mints a short-lived bearer from it and authenticates the fetch (the same
  account that owns the sheet). Steady ~5s, relaxed to ~30s while the WSS is alive.
- **WSS push** (`game-log-api-live.dndbeyond.com`) — needs that same **Cobalt
  cookie** plus the character's `userId`/`gameId`. Every frame nudges an immediate
  refetch. If WSS is unavailable, polling tightens back up — never a hard dependency.
- **Cobalt cookie format** — paste the bare `CobaltSession` *value* (the `eyJ…`
  blob from devtools); the server adds the `CobaltSession=` name for you. It
  expires after a few weeks — re-paste when a private sheet goes stale.
- HP math is a faithful port of the firmware's `hp.py` (see `src/hp.ts`).

## Config (env)

| var | default | meaning |
|-----|---------|---------|
| `PORT` | `8080` | listen port |
| `HOST` | `0.0.0.0` | bind address |
| `DB_PATH` | `./data/tracker.db` | SQLite file (mount a volume in Docker) |
| `ADMIN_PASSWORD` | _(unset)_ | if set, admin pages/forms require `?key=` / a `key` field |

## Docker

```bash
docker compose up --build
```

The DB persists in the `tracker-data` volume.

### ⚠️ Reverse proxy / TLS

The Pico does **plain HTTP only** — no TLS on device. Whatever fronts
`dndhealth.willflix.org` **must serve `/*.txt` over `http://` (port 80) without a
forced HTTPS redirect**, or the devices can't fetch. The admin UI can be
HTTPS-only and/or auth-gated; only `/*` must stay reachable over plain HTTP.

## Security note

The admin UI sets the Cobalt cookie (a full DDB account credential). It has no
strong built-in auth — keep it behind your proxy's auth or on a trusted network.
`ADMIN_PASSWORD` is only a light guard. The cookie is stored in SQLite and never
rendered back.
