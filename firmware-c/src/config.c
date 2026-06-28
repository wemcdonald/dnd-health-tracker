/**
 * config.c — flash-backed persistence (M2). See config.h.
 */
#include "config.h"

#include <string.h>
#include <stddef.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/flash.h"
#include "hardware/sync.h"

/* Set once core1 is running as a lockout victim (see config_set_core1_running).
 * Flash erase/program must stop core1 (which executes from XIP) while it runs. */
static volatile bool s_core1_running = false;
void config_set_core1_running(bool running) { s_core1_running = running; }
bool config_core1_running(void) { return s_core1_running; }

#define CONFIG_MAGIC   0xD2D17EA1u
#define CONFIG_VERSION 1

/* Legacy: pre-OTA config lived in the very last flash sector.
 * PICO_FLASH_SIZE_BYTES and FLASH_SECTOR_SIZE come from the Pico SDK hardware
 * headers included above, so the macro is safe here. */
#define CONFIG_LEGACY_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)

/* Bytes to program: sizeof(persist_t) rounded up to a flash page (256). */
#define CONFIG_PROG_SIZE \
    (((sizeof(persist_t)) + FLASH_PAGE_SIZE - 1) & ~((size_t)FLASH_PAGE_SIZE - 1))

/* Standard CRC-32 (poly 0xEDB88320). */
static uint32_t crc32_calc(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return ~crc;
}

/* Validate and copy a persist_t from a flash offset into *out.
 * Returns true only if magic, version, net_count, and CRC32 all pass. */
static bool read_at(uint32_t offset, persist_t *out) {
    const persist_t *f = (const persist_t *)(XIP_BASE + offset);
    if (f->magic != CONFIG_MAGIC) return false;
    if (f->version != CONFIG_VERSION) return false;
    /* 0 nets is valid (e.g. `set name` before `set wifi`) — caller decides what
     * to do with no networks (we raise the portal). */
    if (f->net_count > MAX_NETS) return false;
    uint32_t want = crc32_calc((const uint8_t *)f, offsetof(persist_t, crc32));
    if (want != f->crc32) return false;
    memcpy(out, f, sizeof(*out));
    return true;
}

bool config_load(persist_t *out) {
    /* Normal path: config partition holds a valid image. */
    if (read_at(CONFIG_FLASH_OFFSET, out)) return true;

    /* First partitioned boot: migrate legacy last-sector copy forward. */
    if (read_at(CONFIG_LEGACY_OFFSET, out)) {
        config_save(out);   /* writes to CONFIG_FLASH_OFFSET; legacy untouched */
        return true;
    }

    /* Neither location valid — provisioning needed. */
    return false;
}

bool config_save(persist_t *cfg) {
    cfg->magic = CONFIG_MAGIC;
    cfg->version = CONFIG_VERSION;
    cfg->crc32 = crc32_calc((const uint8_t *)cfg, offsetof(persist_t, crc32));

    /* Page-aligned, 0xFF-padded image (erased flash reads as 0xFF). */
    static uint8_t buf[CONFIG_PROG_SIZE];
    memset(buf, 0xFF, sizeof(buf));
    memcpy(buf, cfg, sizeof(*cfg));

    /* If core1 is running it executes from XIP flash, which must be quiescent
     * during erase/program. Every config_save is immediately followed by a
     * reboot, so we simply STOP core1 (reset it) rather than do the
     * lockout-handshake dance (which deadlocked on hardware). After this only
     * core0 runs, so disabling core0 IRQs makes the flash op safe. */
    if (s_core1_running) multicore_reset_core1();
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(CONFIG_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(CONFIG_FLASH_OFFSET, buf, CONFIG_PROG_SIZE);
    restore_interrupts(ints);

    /* Verify the exact location just written. NOT config_load() — that now does
     * legacy→partition migration, so on a failed partition write it would fall
     * back to legacy and re-enter config_save, recursing into an infinite
     * flash-erase/program loop. read_at is non-recursive by construction. */
    persist_t check;
    return read_at(CONFIG_FLASH_OFFSET, &check);
}
