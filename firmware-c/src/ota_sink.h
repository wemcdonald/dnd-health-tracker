#ifndef OTA_SINK_H
#define OTA_SINK_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define OTA_SECTOR 4096

typedef bool (*ota_sector_write_fn)(void *ctx, uint32_t sector_index, const uint8_t buf[OTA_SECTOR]);

typedef struct {
    ota_sector_write_fn write;
    void *ctx;
    uint8_t buf[OTA_SECTOR];  // 4 KB; do not stack-allocate ota_sink_t in an interrupt context
    size_t fill;
    uint32_t sector_index;
    bool failed;              // sticky; cleared only by re-calling ota_sink_init
} ota_sink_t;

void ota_sink_init(ota_sink_t *s, ota_sector_write_fn write, void *ctx);
bool ota_sink_push(ota_sink_t *s, const uint8_t *data, size_t len);
bool ota_sink_finish(ota_sink_t *s);
#endif
