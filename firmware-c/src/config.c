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

#define CONFIG_MAGIC   0xD2D17EA1u
#define CONFIG_VERSION 1

/* Last sector of flash, away from the program image. */
#define FLASH_TARGET_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)

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

bool config_load(persist_t *out) {
    const persist_t *flash = (const persist_t *)(XIP_BASE + FLASH_TARGET_OFFSET);
    if (flash->magic != CONFIG_MAGIC) return false;
    if (flash->version != CONFIG_VERSION) return false;
    if (flash->net_count == 0 || flash->net_count > MAX_NETS) return false;
    uint32_t want = crc32_calc((const uint8_t *)flash, offsetof(persist_t, crc32));
    if (want != flash->crc32) return false;
    memcpy(out, flash, sizeof(*out));
    return true;
}

bool config_save(persist_t *cfg) {
    cfg->magic = CONFIG_MAGIC;
    cfg->version = CONFIG_VERSION;
    cfg->crc32 = crc32_calc((const uint8_t *)cfg, offsetof(persist_t, crc32));

    /* Page-aligned, 0xFF-padded image (erased flash reads as 0xFF). */
    static uint8_t buf[CONFIG_PROG_SIZE];
    memset(buf, 0xFF, sizeof(buf));
    memcpy(buf, cfg, sizeof(*cfg));

    /* If core1 is running it executes from XIP flash, so it must be parked
     * during the erase/program. Lock it out first, then disable core0 IRQs. */
    if (s_core1_running) multicore_lockout_start_blocking();
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(FLASH_TARGET_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(FLASH_TARGET_OFFSET, buf, CONFIG_PROG_SIZE);
    restore_interrupts(ints);
    if (s_core1_running) multicore_lockout_end_blocking();

    persist_t check;
    return config_load(&check);
}
