# OTA on-device acceptance (Task 13)

On-device verification of the real OTA round-trip + fault injection, against the
live server (`dndhealth.willflix.org:80`, plain HTTP, firmware paths exempt from
the HTTPS-redirect/Authentik proxy). Publish path: `just publish-fw version=N` →
commit `server/firmware/{image.bin,manifest.txt}` → server agent `git pull` +
copy into `/willflix/docker/appdata/dnd-health-firmware/` (live, no restart).

TBYB mechanism itself is already hardware-proven (see `SPIKE_NOTES.md`); this file
covers the integrated firmware+server round-trip.

## Environment / setup state

| item | value |
|------|-------|
| Branch | `wemcdonald/ota-firmware` |
| Device layout | A @ 0x2000, B @ 0x200000, config @ 0x3FE000 (PT resident) |
| migrate-ota | v1 (non-TBYB) flashed to slot A — DONE |
| Published images | v1 (432280 B, sha256 9dd859…3ffb2, TBYB-flagged) committed |
| Observability | wifi status page (`healthbar-<slug>.local`) + `picotool` in BOOTSEL; USB-CDC serial output is dead on this Mac |
| Device slug / wifi | _TBD — provision_ |

## Acceptance matrix

| # | Case | Expected | Result |
|---|------|----------|--------|
| 0 | Baseline: device on v1, server `latest`=v1 | No update offered (v1 not newer than v1); HP poll continues | _pending_ |
| 1 | Happy path: publish v2, power-cycle | fetch manifest → download v2 → sha256 ok → trial-boot v2 → first good HP poll → commit; next power-cycle stays v2 | _pending_ |
| 2 | Corrupt image rejected | flip a byte in served image so sha mismatches; power-cycle → `sha256 mismatch, aborting`; stays on current; inactive slot never armed | _pending_ |
| 3 | Trial that never buys reverts | v3 that returns before first poll; trial boots v3, never buys; next power-cycle back on v2 | _pending_ |
| 4 | Power-loss mid-download | publish v3, pull power during download; next boot: clean current image, retries, completes | _pending_ |
| 5 | Power-loss after arm, before buy | publish v3, let it verify+reboot into trial, pull power before first poll commits; next boot auto-reverts | _pending_ |
| 6 | NEW edge: TBYB image reboots before first buy w/ empty/old other slot | fails safe — stays on current, no brick / boot-loop | _pending_ |
| 7 | Recovery escape hatch | `picotool load <known-good>.uf2 -p 0 -f -x` (or BOOTSEL + `picotool erase`) recovers regardless of slot state | _pending_ |

## Run log

(chronological notes, commands, observed state per case)
