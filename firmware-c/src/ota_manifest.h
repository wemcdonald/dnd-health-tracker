#ifndef OTA_MANIFEST_H
#define OTA_MANIFEST_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct {
    uint32_t version;
    uint32_t size;
    char     sha256[65];
    char     path[80];
} ota_manifest_t;

bool ota_manifest_parse(const char *body, size_t len, ota_manifest_t *out);
bool ota_is_newer(uint32_t latest, uint32_t running);
#endif
