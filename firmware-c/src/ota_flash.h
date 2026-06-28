#ifndef OTA_FLASH_H
#define OTA_FLASH_H
#include <stdint.h>
#include <stdbool.h>
#include "ota_sink.h"   // OTA_SECTOR

// A/B slot offsets (verified on-device via ota/partition_table.json).
#define SLOT_A_OFFSET 0x002000u
#define SLOT_B_OFFSET 0x200000u
#define OTA_SLOT_SIZE 0x1F2000u   // 1992 KiB — capacity of each slot

// Flash offset of the slot we are NOT currently executing from.
uint32_t ota_inactive_slot_offset(void);

// Erase the inactive slot for `bytes` (rounded up to a sector). Returns false if
// `bytes` exceeds the slot capacity. Call before streaming.
bool ota_flash_prepare(uint32_t slot_offset, uint32_t bytes);

// ota_sector_write_fn: ctx is a `uint32_t*` holding the slot base offset.
bool ota_flash_write_sector(void *ctx, uint32_t sector_index, const uint8_t buf[OTA_SECTOR]);

// Read `len` bytes from slot_offset+off into out (XIP-mapped read, for re-hash).
// Bounds are the caller's responsibility — this function performs no range checks.
void ota_flash_read(uint32_t slot_offset, uint32_t off, uint8_t *out, uint32_t len);

#endif /* OTA_FLASH_H */
