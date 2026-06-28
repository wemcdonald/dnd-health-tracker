/*
 * ota.c — on-device OTA update client (Task 9).
 *
 * Orchestrates: fetch the manifest from /firmware/latest → if newer, stream the
 * image straight into the inactive A/B slot while hashing it → re-hash the image
 * back out of flash and compare to the manifest digest → on a match, arm a
 * Try-Before-You-Buy (TBYB) flash-update reboot of that slot. The freshly-written
 * slot is NEVER armed unless its whole-image SHA-256 matches, so a corrupt/partial
 * download leaves us running the current image.
 *
 * COMPILE-VERIFIED ONLY. No hardware in the loop yet. See the HAZARDS block at the
 * bottom of this file for every runtime risk that cannot be proven without a device.
 */

#include "ota.h"

#include <string.h>
#include <stdio.h>

#include "http_poll.h"
#include "ota_manifest.h"
#include "ota_sink.h"
#include "ota_flash.h"
#include "sha256.h"

#include "pico/bootrom.h"
#include "boot/picoboot_constants.h"   /* REBOOT2_FLAG_REBOOT_TYPE_FLASH_UPDATE */

/* Real value supplied by Task 10 via CMake (pico_set_program_version + a
 * -DFIRMWARE_VERSION define). 0 here means "always behind", which is the safe
 * default for an un-stamped local build (it will accept any published image). */
#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION 0
#endif

/* Download sink: each body chunk is both fed to the running SHA-256 and pushed to
 * the flash sink (which buffers into 4 KB sectors and programs them).
 *
 * HAZARD (flash-write inside the lwIP recv callback): dl_chunk runs synchronously
 * inside http_get_stream's recv callback. When ota_sink_push completes a 4 KB
 * sector it calls ota_flash_write_sector, which disables interrupts and runs
 * flash_range_program (XIP is stalled for the duration). The flash routines run
 * from RAM with IRQs off — the same pattern config.c already uses on this device —
 * so it is *safe*, but it STALLS the network stack for the duration of each sector
 * program (~ms). lwIP's window will simply pause; the server is not generating new
 * data we must service mid-program. Acceptable for a one-shot OTA download. */
typedef struct {
    ota_sink_t sink;
    sha256_ctx sha;
    bool       ok;
} dl_ctx_t;

static bool dl_chunk(void *vctx, const uint8_t *data, size_t len) {
    dl_ctx_t *c = (dl_ctx_t *)vctx;
    sha256_update(&c->sha, data, len);
    if (!ota_sink_push(&c->sink, data, len)) {
        c->ok = false;
        return false;   /* aborts the transfer */
    }
    return true;
}

ota_result_t ota_check_and_update(const ip_addr_t *srv, uint16_t port, const char *host) {
    /* 1. Fetch the manifest. Not-published / unreachable is NON-fatal: stay on the
     *    current image and try again next cycle. */
    char body[256];
    int n = http_get_body(srv, port, host, "/firmware/latest", body, sizeof(body), 4000);
    if (n <= 0) return OTA_NONE;

    /* 2. Parse + version-gate. */
    ota_manifest_t m;
    if (!ota_manifest_parse(body, (size_t)n, &m)) return OTA_ERROR;
    if (!ota_is_newer(m.version, FIRMWARE_VERSION)) return OTA_NONE;

    printf("OTA: update v%lu (%lu bytes) available; downloading\n",
           (unsigned long)m.version, (unsigned long)m.size);

    /* 3. Erase the inactive slot.
     *
     * HAZARD (core1 reset): ota_flash_prepare calls multicore_reset_core1() if
     * core1 is running. core1 must NOT be restarted between here and the
     * reboot/return below — it would race the flash programming. That is the
     * caller's responsibility (Task 10): stop core1 / the LED renderer before
     * calling ota_check_and_update and do not restart it on the OTA_ARMED path. */
    static uint32_t slot;                 /* static: ota_flash_write_sector ctx is &slot */
    slot = ota_inactive_slot_offset();
    if (!ota_flash_prepare(slot, m.size)) return OTA_ERROR;

    /* 4. Wire up the streaming sink. static — the sink holds a 4 KB sector buffer
     *    we keep off the stack. */
    static dl_ctx_t dl;
    ota_sink_init(&dl.sink, ota_flash_write_sector, &slot);
    sha256_init(&dl.sha);
    dl.ok = true;

    /* 5. Stream the image into flash (30 s budget; large transfer over a stalling
     *    link). */
    int got = http_get_stream(srv, port, host, m.path, dl_chunk, &dl, 30000);
    if (got < 0 || !dl.ok) {
        printf("OTA: download failed (got=%d ok=%d)\n", got, (int)dl.ok);
        return OTA_ERROR;
    }
    if (!ota_sink_finish(&dl.sink)) {
        printf("OTA: flash flush failed\n");
        return OTA_ERROR;
    }
    if ((uint32_t)got != m.size) {
        printf("OTA: size mismatch (got %d, manifest %lu)\n", got, (unsigned long)m.size);
        return OTA_ERROR;
    }

    /* 6. Re-hash the image straight from flash and compare to the manifest digest.
     *    This proves what is ACTUALLY on the slot (not just what we streamed) is
     *    intact before we let the bootrom trial-boot it.
     *
     * `rb` is a 4 KB sector-sized re-hash buffer kept static (off the portal task
     * stack) so the OTA path adds no stack pressure — safe because the OTA check is
     * single-shot / non-reentrant (one OTA at a time), like `slot` and `dl`. */
    static uint8_t rb[OTA_SECTOR];
    sha256_ctx vh;
    sha256_init(&vh);
    for (uint32_t off = 0; off < m.size; off += OTA_SECTOR) {
        uint32_t chunk = (m.size - off) < (uint32_t)OTA_SECTOR ? (m.size - off)
                                                               : (uint32_t)OTA_SECTOR;
        ota_flash_read(slot, off, rb, chunk);
        sha256_update(&vh, rb, chunk);
    }
    uint8_t digest[32];
    char hex[65];
    sha256_final(&vh, digest);
    sha256_hex(digest, hex);

    if (strcmp(hex, m.sha256) != 0) {
        printf("OTA: sha256 mismatch\n  got %s\n  exp %s\n", hex, m.sha256);
        return OTA_ERROR;   /* inactive slot NOT armed — stay on the current image */
    }

    /* 7. Verified. Arm a TBYB trial boot of the freshly-written slot.
     *
     * OPEN QUESTION (MUST be confirmed on hardware): rom_reboot's param0
     * (update_base) — is it the flash STORAGE offset (e.g. 0x200000) or the
     * XIP-mapped address (0x10200000)? We pass the storage offset (`slot`) per the
     * spike's current assumption. If the trial boot does not pick the new slot on
     * hardware, try (XIP_BASE | slot). See ota/SPIKE_NOTES.md.
     *
     * The 100 ms delay lets this printf flush; rom_reboot returns (no
     * NO_RETURN_ON_SUCCESS flag), so the caller is expected to spin feeding the
     * watchdog until the reboot fires. */
    printf("OTA: verified v%lu — arming TBYB reboot of slot 0x%lx\n",
           (unsigned long)m.version, (unsigned long)slot);
    rom_reboot(REBOOT2_FLAG_REBOOT_TYPE_FLASH_UPDATE, 100, slot, 0);
    return OTA_ARMED;
}

void ota_commit_if_trial(void) {
    /* Commit (buy) the running image so the bootrom stops treating it as a trial
     * and won't auto-revert on the next watchdog/reset. Idempotent and harmless if
     * the running image is not actually a trial — rom_explicit_buy is a no-op then.
     *
     * The workarea must be 4096 bytes and aligned; the bootrom uses it as scratch. */
    static __attribute__((aligned(4))) uint8_t workarea[4096];
    int r = rom_explicit_buy(workarea, sizeof(workarea));
    if (r == 0) printf("OTA: committed\n");
}

/* ── HAZARDS (runtime risks not provable without hardware) ─────────────────────
 *
 * 1. lwIP streaming correctness: http_get_stream's pbuf-chain walk, header/body
 *    split accounting, and tcp_recved backpressure are only compile-checked. The
 *    exact byte accounting at the header/body boundary (esp. a "\r\n\r\n" that
 *    straddles two pbufs) needs a real transfer to confirm no bytes are dropped or
 *    double-counted.
 * 2. Flash write inside the recv callback stalls the network during each ~4 KB
 *    sector program. Believed safe (RAM-resident flash funcs, IRQs off, matches
 *    config.c) but unproven under live TCP — the window pause could in theory
 *    trip a server/socket timeout on a slow link.
 * 3. update_base form (storage offset vs XIP address) for rom_reboth — see step 7.
 * 4. Watchdog interaction across a ~30 s download: http_get_stream feeds the
 *    watchdog in its poll loop, but the watchdog is NOT fed during a flash sector
 *    program (IRQs off, no poll). A single sector program is well under any sane
 *    watchdog period, but the cumulative behaviour under a flaky link is unproven.
 * 5. (resolved) The 4 KB re-hash buffer `rb` is now static, so it adds no portal
 *    task stack pressure. The static download/sink state (`slot`, `dl`, `rb`, the
 *    streaming ctx) relies on OTA being single-shot / non-reentrant.
 * 6. core1 must stay stopped between ota_flash_prepare and the reboot (caller's
 *    job — Task 10).
 */
