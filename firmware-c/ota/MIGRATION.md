# One-time migration to the A/B OTA layout

Converts a device flashed with the old single-image firmware to the RP2350 A/B
partition layout used by OTA. After this, the device updates itself over WiFi at
boot. **Run once per device.** Config (WiFi + slug) auto-migrates from the legacy
last-sector on the first partitioned boot — you do not re-provision unless you choose to.

## Layout this installs (verified on-device; see ota/SPIKE_NOTES.md)
```
partition 0  "A"       0x002000 → 0x1F4000   (1992 KiB, app slot A)
partition 1  "B" w/ 0  0x200000 → 0x3F2000   (1992 KiB, app slot B, A/B-linked)
partition 2  "config"  0x3FE000 → 0x400000   (8 KiB, data — config lives here)
```
Spec: `ota/partition_table.json`.

## Before you start (back up — there is no `just show`)
The firmware is generic; identity (WiFi nets + slug) is provisioned at runtime and
persisted in flash. The migration preserves it (auto-migrates the legacy sector), but
back it up anyway in case you need to re-provision:
1. While the device is running normally, open its status page at
   `http://healthbar-<slug>.local` (or the device IP) and note the slug + WiFi SSIDs.
2. You re-provision later, if needed, with `just set name <slug>` and
   `just set wifi <SSID> '<pw>'`.

## Migrate
Connect the device over USB in **BOOTSEL** mode (hold BOOTSEL while plugging in, or
`picotool reboot -u -f` from a running, reachable device), then:
```bash
just migrate-ota              # builds v1 + installs PT + flashes slot A, then runs
# or pin a starting version:
just migrate-ota version=1
```
What the recipe does (all picotool steps pinned in ota/SPIKE_NOTES.md):
1. `just build version=<n>` — builds `m1_portal` with `FIRMWARE_VERSION=<n>` (no device needed).
2. `picotool partition create ota/partition_table.json build/pt.uf2`.
3. `picotool load build/pt.uf2 -f` — writes the partition table.
4. `picotool reboot -u` — **required**: the bootrom only registers a newly-written PT
   after a reboot (a fresh `picotool partition info` right after the load shows
   "there is no partition table" until you reboot).
5. `picotool load build/m1_portal.uf2 -p 0 -f -x` — flashes slot A (partition **index 0**;
   picotool 2.x wants the index, not the name "A") and starts it.

## Expected first boot
- Serial/log: `config_load` finds nothing valid at the config partition (0x3FE000),
  falls back to the legacy sector (0x3FF000), finds the old config, and migrates it
  forward (one `config_save` to 0x3FE000). Subsequent boots read the partition directly.
- The device connects WiFi and resumes normal HP tracking + LED bar.
- It performs a boot-only OTA check against `/firmware/latest`; on first run the server
  has no newer image (or none published), so it just runs normally.

## Verify
- Device tracks HP and the LED bar works (open the status page).
- Confirm the same slug/WiFi as before migration (status page).
- `picotool partition info` (device in BOOTSEL) shows the three partitions.

## Recovery escape hatch (if the device won't boot or goes unresponsive)
The migration is **not** a brick risk — the bootrom + USB recovery always work:
1. **Force BOOTSEL:** hold the BOOTSEL button while plugging in USB. (`picotool reboot -u -f`
   also works *if* the running image is still reachable over USB.)
2. **Reflash a known-good image into slot A:**
   `picotool load build/m1_portal.uf2 -p 0 -f -x` (or a saved good `.uf2`).
3. **Full reset:** `picotool erase` wipes the chip (clears any corrupt config/PT), then
   re-run `just migrate-ota`. A corrupted config sector can survive a normal reflash —
   `picotool erase` is the definitive reset.

### Known USB caveat on the dev Mac (from the spike)
USB-CDC enumeration for the app build is unreliable on the current dev Mac — after an
app starts, `/dev/cu.usbmodem*` and even `picotool info -f` may not appear, making the
running device unreachable without a physical power-cycle / BOOTSEL-hold. If a `migrate-ota`
step that talks to a *running* device hangs, recover via BOOTSEL-hold and re-run from the
`picotool load build/pt.uf2` step. Prefer driving migration with the device in BOOTSEL
throughout (the recipe's `reboot -u` keeps it in BOOTSEL between steps).
