// SPIKE (throwaway — Task 1): prove RP2350 A/B + try-before-you-buy (TBYB).
//
// Observation: NO USB-CDC (unreliable on this Mac). TWO mechanisms:
//  1) Each boot writes a marker to the config partition (0x3FE000): bytes
//     {0xAA, FIRMWARE_VERSION, ~FIRMWARE_VERSION, 0x55}. Read back in BOOTSEL with
//     `picotool save -r 0x103FE000 0x103FE010 f.bin` → tells which SLOT's code last ran
//     (v1=slot A, v2=slot B). This is ground truth for which image actually booted, and
//     also proves flash_range_* works with a resident partition table.
//  2) Each variant returns to BOOTSEL via rom_reboot(BOOTSEL) so picotool can read state.
//
// Variants: FIRMWARE_VERSION=1 (slot A; arms a FLASH_UPDATE trial of B once per
// power-cycle via scratch[0] guard). FIRMWARE_VERSION=2 + SPIKE_BUY=0/1 (slot B trial;
// buy=1 calls rom_explicit_buy). Deleted at end of Task 1.
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "boot/picoboot_constants.h"
#include "hardware/watchdog.h"
#include "hardware/flash.h"
#include "hardware/sync.h"

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION 1
#endif
#ifndef SPIKE_BUY
#define SPIKE_BUY 0
#endif
#ifndef SPIKE_ARM
#define SPIKE_ARM 0      // v1: 1 = self-arm a FLASH_UPDATE trial of B; 0 = passive (mark+bootsel)
#endif

#define SLOT_B_OFFSET 0x200000u
#define MARK_OFFSET   0x3FE000u     // config partition first sector
#define ARMED_MAGIC   0xA5A5A5A5u
#ifndef SPIKE_UPDATE_BASE
#define SPIKE_UPDATE_BASE SLOT_B_OFFSET   // storage offset; override with 0x10200000 (XIP) to test
#endif

static void mark(uint8_t v) {
    uint8_t page[FLASH_PAGE_SIZE];
    for (int i = 0; i < (int)FLASH_PAGE_SIZE; i++) page[i] = 0xFF;
    page[0] = 0xAA; page[1] = v; page[2] = (uint8_t)~v; page[3] = 0x55;
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(MARK_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(MARK_OFFSET, page, FLASH_PAGE_SIZE);
    restore_interrupts(ints);
}

static void to_bootsel(void) {
    rom_reboot(REBOOT2_FLAG_REBOOT_TYPE_BOOTSEL, 100, 0, 0);
    while (true) tight_loop_contents();
}

int main(void) {
    sleep_ms(1500);
    mark(FIRMWARE_VERSION);   // record that THIS image booted (last writer wins)

#if SPIKE_ARM
    /* Arm a TBYB flash-update trial of the slot at SPIKE_UPDATE_BASE (XIP address),
     * once per power-cycle (scratch guard breaks the revert loop). */
    if (watchdog_hw->scratch[0] != ARMED_MAGIC) {
        watchdog_hw->scratch[0] = ARMED_MAGIC;
        rom_reboot(REBOOT2_FLAG_REBOOT_TYPE_FLASH_UPDATE, 100, SPIKE_UPDATE_BASE, 0);
        while (true) tight_loop_contents();
    } else {
        sleep_ms(300);
        to_bootsel();          /* reached again => reverted here; show it */
    }
#else
  #if SPIKE_BUY
    static __attribute__((aligned(4))) uint8_t wa[4096];
    rom_explicit_buy(wa, sizeof(wa));   /* commit this trial */
  #endif
    sleep_ms(300);
    to_bootsel();
#endif
    while (true) tight_loop_contents();
}
