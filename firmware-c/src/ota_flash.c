/*
 * ota_flash.c — inactive-slot flash writer (core1-safe erase/program)
 *
 * Erase and program the inactive A/B slot using flash_range_erase / flash_range_program,
 * matching the pattern already proven in config.c.  The same core1-lockout discipline
 * applies: stop core1 (if running) then disable interrupts around every flash op.
 *
 * Note on RP2350 permission-checked bootrom: if a resident partition table causes the
 * bootrom to reject raw flash_range_* at the target offset (permission violation), the
 * fallback is rom_flash_op / rom_helper_flash_op with the same offsets and sizes.
 * Confirm during on-device bring-up.
 */

#include "ota_flash.h"
#include <string.h>
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/multicore.h"
#include "config.h"   // config_core1_running()

#define XIP_BASE_ADDR 0x10000000u

/* __flash_binary_start is defined by the RP2350 linker scripts (memmap_default.ld et al).
 * Its address is the XIP-mapped load address of this image, from which we derive the
 * running slot. */
extern char __flash_binary_start;

static uint32_t running_slot_offset(void) {
    uint32_t base = (uint32_t)(uintptr_t)(&__flash_binary_start) - XIP_BASE_ADDR;
    return (base >= SLOT_B_OFFSET) ? SLOT_B_OFFSET : SLOT_A_OFFSET;
}

uint32_t ota_inactive_slot_offset(void) {
    return (running_slot_offset() == SLOT_A_OFFSET) ? SLOT_B_OFFSET : SLOT_A_OFFSET;
}

bool ota_flash_prepare(uint32_t slot_offset, uint32_t bytes) {
    /* Round up to the nearest sector boundary — flash_range_erase requires sector alignment. */
    uint32_t span = (bytes + FLASH_SECTOR_SIZE - 1) & ~(uint32_t)(FLASH_SECTOR_SIZE - 1);
    if (span > OTA_SLOT_SIZE) return false;   // never erase past the slot into the next partition

    if (config_core1_running()) multicore_reset_core1();
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(slot_offset, span);
    restore_interrupts(ints);
    return true;
}

bool ota_flash_write_sector(void *ctx, uint32_t sector_index, const uint8_t buf[OTA_SECTOR]) {
    uint32_t base = *(const uint32_t *)ctx;
    uint32_t off  = base + sector_index * (uint32_t)OTA_SECTOR;

    /* Guard: stay within the slot. */
    if (off + (uint32_t)OTA_SECTOR > base + OTA_SLOT_SIZE) return false;

    /* core1 was already stopped in ota_flash_prepare; no need to stop it again.
     * We still disable interrupts — flash_range_program requires no XIP activity. */
    uint32_t ints = save_and_disable_interrupts();
    flash_range_program(off, buf, OTA_SECTOR);
    restore_interrupts(ints);
    return true;
}

void ota_flash_read(uint32_t slot_offset, uint32_t off, uint8_t *out, uint32_t len) {
    /* XIP-mapped read. Bounds are the caller's responsibility. */
    const uint8_t *p = (const uint8_t *)(uintptr_t)(XIP_BASE_ADDR + slot_offset + off);
    memcpy(out, p, len);
}
