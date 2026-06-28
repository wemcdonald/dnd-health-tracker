// SPIKE (throwaway — Task 1): prove RP2350 A/B + try-before-you-buy (TBYB).
//
// Same source built into several variants via -D flags:
//   FIRMWARE_VERSION = 1  -> the "old" image, flashed into slot A. After a few
//                           heartbeats it arms a FLASH_UPDATE (trial) boot of slot B.
//                           Guarded by watchdog scratch so it does NOT re-arm after a
//                           revert (scratch survives reset, is cleared by power loss).
//   FIRMWARE_VERSION = 2  -> the "new" trial image, flashed into slot B.
//       SPIKE_BUY = 0     -> never calls explicit_buy  (proves trial auto-reverts)
//       SPIKE_BUY = 1     -> calls explicit_buy on boot (proves commit sticks)
//
// Deleted at end of Task 1 — do not depend on this file.
#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "boot/picoboot_constants.h"   // REBOOT2_FLAG_REBOOT_TYPE_FLASH_UPDATE
#include "hardware/watchdog.h"

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION 1
#endif
#ifndef SPIKE_BUY
#define SPIKE_BUY 0
#endif

// Storage offsets (from ota/partition_table.json): A @ 8k, B @ 2048k.
#define SLOT_A_OFFSET 0x002000u
#define SLOT_B_OFFSET 0x200000u
#define XIP_BASE      0x10000000u

#define ARMED_MAGIC 0xA5A5A5A5u   // scratch[0]: "v1 already armed this power-cycle"

extern char __flash_binary_start;  // linker symbol = where this image is mapped

static const char *which_slot(void) {
    uint32_t off = (uint32_t)(uintptr_t)(&__flash_binary_start) - XIP_BASE;
    if (off >= SLOT_B_OFFSET) return "B";
    if (off >= SLOT_A_OFFSET) return "A";
    return "?";
}

int main(void) {
    stdio_init_all();
    sleep_ms(2500);  // let USB-CDC enumerate before first print
    uint32_t bin_off = (uint32_t)(uintptr_t)(&__flash_binary_start) - XIP_BASE;
    printf("\n==== SPIKE boot: v%d slot=%s bin_off=0x%06x scratch0=0x%08x ====\n",
           FIRMWARE_VERSION, which_slot(), (unsigned)bin_off,
           (unsigned)watchdog_hw->scratch[0]);

#if FIRMWARE_VERSION >= 2
    // Trial image. Optionally commit.
    printf("SPIKE v2 TRIAL BOOT (SPIKE_BUY=%d)\n", SPIKE_BUY);
  #if SPIKE_BUY
    static __attribute__((aligned(4))) uint8_t workarea[4096];
    int r = rom_explicit_buy(workarea, sizeof(workarea));
    printf("SPIKE explicit_buy -> %d (0 = committed)\n", r);
  #else
    printf("SPIKE not buying -> expect revert to v1 on next reset\n");
  #endif
#else
    // Old image in A. Arm a flash-update trial of B once per power-cycle.
    if (watchdog_hw->scratch[0] != ARMED_MAGIC) {
        for (int i = 3; i > 0; i--) { printf("SPIKE v1: arming B trial in %d...\n", i); sleep_ms(1000); }
        watchdog_hw->scratch[0] = ARMED_MAGIC;
        printf("SPIKE v1: rom_reboot(FLASH_UPDATE, base=0x%06x)\n", SLOT_B_OFFSET);
        rom_reboot(REBOOT2_FLAG_REBOOT_TYPE_FLASH_UPDATE, 50, SLOT_B_OFFSET, 0);
    } else {
        printf("SPIKE v1: already armed this power-cycle (REVERTED here) — not re-arming\n");
    }
#endif

    int beats = 0;
    while (true) {
        printf("SPIKE alive v%d slot=%s beat=%d\n", FIRMWARE_VERSION, which_slot(), beats++);
        sleep_ms(1000);
    }
}
