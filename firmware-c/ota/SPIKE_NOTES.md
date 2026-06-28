# Task 1 Spike Notes — RP2350 A/B + TBYB

Status: **COMPLETE (hardware, 2026-06-28).** Full TBYB cycle PROVEN on real hardware:
flash-update **trial boot** (XIP update_base), **explicit-buy commit**, AND **auto-revert**
(un-bought TBYB trial → reverts). **Two real firmware fixes found:** (1) reboot update_base
must be the XIP address — fixed in `0891a50`; (2) OTA image must be TBYB-flagged
(`PICO_CRT0_IMAGE_TYPE_TBYB=1`) or there is no rollback — see RESULT 5. One on-device
edge case left for Task 13 (TBYB image reboot before first buy w/ empty other slot).

## HARDWARE RESULTS (2026-06-28)

Observation method (USB-CDC is unreliable on this Mac, so NO serial): the spike app
(`src/m6_tbyb.c`) writes a 4-byte marker `{0xAA, version, ~version, 0x55}` to the config
partition (0x3FE000) on every boot, and returns itself to BOOTSEL via
`rom_reboot(BOOTSEL)`. Read back in BOOTSEL with
`picotool save -r 0x103FE000 0x103FE004 f.bin` → marker[1] tells which SLOT executed
(1=A, 2=B). A/B active slot read with `picotool partition info -m rp2350-arm-s`
("can be downloaded in partition N" ⇒ the OTHER partition is active/committed).

- **RESULT 1 — update_base MUST be the XIP address (CRITICAL, bug fixed).**
  `rom_reboot(REBOOT2_FLAG_REBOOT_TYPE_FLASH_UPDATE, 100, update_base, 0)` only trials
  the target slot when `update_base = XIP_BASE + slot_offset` (e.g. 0x10200000 for B).
  With the **storage offset** (0x200000) the bootrom kept booting the committed slot A
  (marker stayed 1) — no trial. With the **XIP address** the trial fired (marker→2).
  `ota.c` had passed the storage offset → **fixed in commit 0891a50** (`XIP_BASE + slot`).
- **RESULT 2 — explicit_buy commits (PROVEN).** After the XIP trial of B(v2) called
  `rom_explicit_buy`, the active slot flipped A→B (`partition info -m` target moved from
  partition 1 to partition 0) and stayed. Commit works.
- **RESULT 3 — flash_range_* works with a resident partition table (PROVEN).** The app
  wrote the marker to 0x3FE000 via `flash_range_erase`/`flash_range_program` and it read
  back correctly. So Task 8's `flash_range_*` choice is correct — `rom_flash_op` is NOT
  needed. (No core1 in the spike; the firmware additionally resets core1 first.)
- **RESULT 4 — `picotool reboot -g <part>` does NOT force a slot boot** (it sets the
  diagnostic partition). Forcing a specific slot is done via the FLASH_UPDATE trial only.
- **RESULT 5 — TBYB requires the image-def "explicit buy" flag (CRITICAL, 2nd firmware fix).**
  bootrom.h:912 — TBYB (run-once-then-revert) only applies to an image whose IMAGE_DEF is
  "explicit buy" flagged. WITHOUT the flag a flash-update boot is **permanent immediately**
  (no trial/rollback). My FIRST commit/revert reads were on UNFLAGGED images so BOTH "stuck"
  (the apparent "commit" was permanent-by-default — misattributed). Set the flag at build
  with **`PICO_CRT0_IMAGE_TYPE_TBYB=1`** (crt0 → `PICOBIN_IMAGE_TYPE_EXE_TBYB_BITS`);
  `picotool info <uf2>` then shows `tbyb: not bought`. Re-tested WITH the flag on hardware:
  - **Auto-revert PROVEN:** A(arm,XIP) → trial B(v2 nobuy, TBYB): marker 2 (B ran), active
    stayed A; plain reboot → marker **1** (reverted to A), active A. ✓
  - **Commit PROVEN:** A(arm,XIP) → trial B(v2 buy, TBYB): `explicit_buy` → active flipped to
    **B** (download target → partition 0); plain reboot → stayed B (marker 2). ✓
  → **Firmware requirement:** the OTA-PUBLISHED image MUST be built TBYB-flagged so a bad
  update reverts. Keep the INITIAL migrate-install UNFLAGGED (permanent immediately) to dodge
  a revert-to-empty-slot risk before the first commit. `ota_commit_if_trial()` (explicit_buy
  after first good poll) commits the trial. STILL TO VERIFY on-device (Task 13): a TBYB-flagged
  image that reboots BEFORE the first buy, with an empty/old other slot.
- **Single-image-vs-per-slot:** the SAME built image runs from whichever slot the
  bootrom selects (v1 ran from A, v2 from B, same source) → single-image-via-address-
  translation confirmed; no per-slot relink needed.

---

### (Superseded) original blocker note

## Step 1 — Pinned tool versions (verified 2026-06-27)
- `picotool` **2.2.0** (Homebrew, `/opt/homebrew/Cellar/picotool/2.2.0`).
- Pico SDK **2.2.0** (`/Users/will/code/pico-sdk`, `PICO_SDK_PATH`).
- Both ≥ the plan's floor (picotool ≥2.0, SDK ≥2.1): partition tables + `rom_explicit_buy` supported.

## Step 2 — Partition-table JSON schema (CORRECTED vs plan)
The plan's `partition_table.json` used fields that **do not exist** in picotool 2.2.0's
schema (`json/schemas/partition-table-schema.json`). Corrected `ota/partition_table.json`:
- **Removed** `"ab": true` and `"image_type"` — neither is in the schema. (Initial error:
  `type must be array, but is null`.)
- **A/B is expressed by `"link": ["a", 0]` on the B partition** (link type enum
  `["a","owner","none"]`, value = the partition index it pairs with). My original
  `["a",0]` was correct.
- **`"families"` is REQUIRED** on `unpartitioned` and every partition (it was the missing
  null array). App slots → `["rp2350-arm-s","rp2350-riscv"]`; data → `["data"]`;
  unpartitioned → `["absolute"]`.
- App-vs-data is the `families` array, NOT an `image_type` field.
- `start`/`size` use the `^\d+(k|K)$` form (e.g. `"8k"`, `"1992k"`); explicit starts pin
  the layout deterministically.

## Step 3 — Partition table builds + lands (PROVEN on device)
- `picotool partition create ota/partition_table.json build/pt.uf2` → exit 0.
- `picotool partition info build/pt.uf2` is **NOT** valid (plan error) — `partition info`
  is **device-only**. To inspect a created PT file use `picotool info build/pt.uf2`.
- `picotool load build/pt.uf2` writes it (family `absolute`).
- **GOTCHA: the PT is not recognised until the device REBOOTS.** Immediately after the
  load, `picotool partition info` said "there is no partition table"; after
  `picotool reboot -u`, it read back correctly.
- **Verified on-device layout** (matches the planned layout exactly):
  ```
  0(A)       00002000->001f4000  "A"      uf2 { rp2350-arm-s, rp2350-riscv }
  1(B w/ 0)  00200000->003f2000  "B"      uf2 { rp2350-arm-s, rp2350-riscv }   (linked to A)
  2          003fe000->00400000  "config" uf2 { data }
  ```
  So: **A @ 0x002000, B @ 0x200000, config @ 0x3FE000** — these are the constants for
  Tasks 8 (slot offsets) and 11 (CONFIG_FLASH_OFFSET = 0x3FE000). Each app slot is
  0x1F2000 = 1992 KiB.

## picotool invocation corrections (vs plan text)
- `-p` takes the partition **INDEX**, not the name: `-p 0` = A, `-p 1` = B. (`-p A`
  errors: "A is not a valid integer".)
- `picotool load <uf2> -p 0 -x` loaded into A and "rebooted to start the application"
  (exit 0) — the write path works.

## rom_reboot / explicit_buy API (pinned from SDK headers)
- `boot/picoboot_constants.h`: **`REBOOT2_FLAG_REBOOT_TYPE_FLASH_UPDATE = 0x4`**,
  param0 = `update_base`. `REBOOT2_FLAG_REBOOT_TYPE_NORMAL = 0x0`,
  `REBOOT2_FLAG_NO_RETURN_ON_SUCCESS = 0x100`.
- `pico/bootrom.h`: `rom_reboot(uint32_t flags, uint32_t delay_ms, uint32_t p0, uint32_t p1)`.
  For a flash-update (TBYB) trial boot of a slot:
  `rom_reboot(REBOOT2_FLAG_REBOOT_TYPE_FLASH_UPDATE, delay_ms, <update_base>, 0)`.
  Doc: "if p0 matches the start address of a partition/slot, that slot is treated
  preferentially during boot … facilitates TBYB and version downgrades."
  **UNRESOLVED empirically:** whether `update_base` is the flash **storage offset**
  (e.g. 0x200000) or the XIP-mapped address (0x10200000). Spike app currently passes the
  storage offset; confirm on hardware.
- `rom_explicit_buy(uint8_t *buffer, uint32_t buffer_size)` — buffer should be a
  4096-byte aligned workarea; returns 0 on success (commit the trial).
- `rom_pick_ab_partition(workarea, size, partition_a_num, flash_update_boot_window_base)`
  exists; the header notes `rom_get_b_partition` / flash-update-base helpers for use
  "before calling rom_explicit_buy" — relevant if address-translation handling is needed.

## CMake helpers available (SDK 2.2.0)
`pico_set_program_version(TARGET "MAJ.MIN")`, `pico_embed_pt_in_binary(TARGET PTFILE)`,
`pico_set_binary_type(TARGET TYPE)`, `pico_set_uf2_family`. The spike used
`pico_set_program_version(m6_tbyb "${FIRMWARE_VERSION}.0")` to stamp the image-def
version so the bootrom A/B picker agrees with `FIRMWARE_VERSION`.

## Single-image-vs-per-slot linking — ASSUMED single-image (NOT yet empirically proven)
A and B share identical `families` and the same UF2 loads into either partition by index;
RP2350 bootrom address translation is the documented mechanism. Proceeding on the plan's
single-image assumption. **Hardware check still owed:** load the SAME uf2 into A and into
B and confirm both run. If per-slot linking turns out to be required, Task 4 (publish) +
Task 9 need a per-slot variant — flagged in the plan's Notes/Risks.

## flash_range_* vs rom_flash_op (for Task 8)
Not yet empirically decided. Plan A: reuse the existing `config.c` pattern
(`flash_range_erase`/`flash_range_program` on absolute offsets, with
`multicore_reset_core1()` + `save_and_disable_interrupts()`), which already works on this
device for the config sector. With a resident PT granting `S(rw) NSBOOT(rw) NS(rw)` on
the slots, absolute-offset `flash_range_*` is expected to work. If the bootrom enforces
partition permissions and rejects raw `flash_range_*`, switch to `rom_flash_op` /
`rom_helper_flash_op` (same offsets/sizes). **Confirm on hardware during Task 8 bring-up.**

## BLOCKER — USB observability on this Mac (queued for human)
After `picotool load build/m6_v1.uf2 -p 0 -x` ("rebooted to start the application"),
the device went **completely USB-invisible**: no `/dev/cu.usbmodem*` (CDC serial),
`picotool info -f` / `reboot -f -u` all report "No accessible RP-series devices",
and `system_profiler SPUSBDataType` shows no 2e8a device. This matches the known hazard
(see memory rearchitecture-status: "USB-CDC serial OUTPUT is unreliable on this Mac").
m6_tbyb has no cyw43, yet still didn't enumerate — root cause undetermined (app may run
fine but not enumerate CDC, or it hangs before USB init, or the partitioned-boot image
needs different packaging).

Consequence: I cannot observe the spike's serial output NOR reach the device to drive the
trial/revert/commit sequence autonomously. **The device is not bricked** — a power-cycle
or BOOTSEL-hold-on-powerup recovers it (then `picotool load <known-good>.uf2 -p 0 -x`
or BOOTSEL drag).

### Spike artifacts ready for the human-assisted run
Built and waiting in `build/`: `pt.uf2`, `m6_v1.uf2` (v1→slot A, self-arms a FLASH_UPDATE
trial of B after 3s), `m6_v2nobuy.uf2` (v2→B, never buys → should revert),
`m6_v2buy.uf2` (v2→B, calls explicit_buy → should commit). Spike source: `src/m6_tbyb.c`,
CMake target `m6_tbyb` (`-DFIRMWARE_VERSION` + `-DSPIKE_BUY`).

### Human-assisted validation procedure (TODO with Will)
1. Recover device to BOOTSEL (hold BOOTSEL while plugging in).
2. `picotool load build/pt.uf2` then `picotool reboot -u` (reboot so PT is seen).
3. Decide observability: if USB-CDC still won't enumerate, add a flash-marker scheme
   (app writes "vN slot X buy Y" into the config data partition; read back via
   `picotool save -p 2 marker.bin` in BOOTSEL) instead of relying on serial. OR use a
   picoprobe/UART for serial (the build already enables UART stdio).
4. Load `m6_v1.uf2 -p 0`, `m6_v2nobuy.uf2 -p 1`; reboot; confirm trial boots v2 then a
   plain `picotool reboot` reverts to v1.
5. Reflash `m6_v2buy.uf2 -p 1`; power-cycle (clears scratch); confirm trial boots v2,
   `explicit_buy -> 0`, and it then STAYS v2 across resets.
6. Confirm the same uf2 runs from both A and B (single-image question).
7. Record the resolved `update_base` form (offset vs XIP) and flash_range vs rom_flash_op.
