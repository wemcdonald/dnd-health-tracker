# `/dnd/<slug>.txt` wire format

Served over **plain HTTP** (no TLS), `Content-Type: text/plain`. The device fetches
`http://<host>/dnd/<slug>.txt` every ~2s.

## The only rule that matters

**Line 1 must be two space-separated integers:** `<lit> <age_s>`

The firmware does exactly:

```c
sscanf(body, "%d %d", &lit, &age_s);   // requires both, and 0 <= lit <= 16
```

- `lit` — number of the 16 LEDs to light. **You precompute it** (the device does no HP math).
  - `lit = round(16 * current_hp / max_hp)`, clamped to `0..16`
  - show **at least 1** if `current_hp > 0` (so "barely alive" ≠ "dead")
  - `0` only when truly down (0 HP)
- `age_s` — seconds since you last successfully refreshed that character from D&D Beyond
  (upstream staleness). Use a big sentinel (e.g. `99999`) if you've never fetched it yet.

Anything after line 1 is **ignored by the device** — put whatever human-readable text you
like there for browser spot-checking.

If the body is missing, non-200, or line 1 doesn't parse as two ints with `0 <= lit <= 16`,
the device treats it as **offline** (breathing animation) — so a 404 or garbage is safe.

## Examples

Healthy (31/32 HP → ~97% → 16 LEDs), refreshed 2s ago:
```
16 2
HP 31/32 (+0 temp) · 97%
```

Bloodied (12/40 → 30% → 5 LEDs):
```
5 1
HP 12/40 · 30%
```

Critical (3/40 → ~7% → 1 LED; device adds the low-HP heartbeat):
```
1 0
HP 3/40 · 7%
```

Down (0 HP → 0 LEDs):
```
0 4
HP 0/40 · DOWN
```

Stale (server hasn't reached D&D Beyond in a while — device can dim/flag it):
```
16 240
HP 31/32 · 97% (stale: 4m since last refresh)
```

Never fetched yet (sentinel age):
```
0 99999
HP unknown (no upstream data yet)
```
