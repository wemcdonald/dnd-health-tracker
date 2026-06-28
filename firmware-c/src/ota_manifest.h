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

// Largest firmware image we will accept. Must not exceed the A/B partition
// slot size (planned 1992 KiB per slot); the flash layer additionally enforces
// the exact slot bound. Rejects absurd/overflowed sizes from an untrusted manifest.
#define OTA_MAX_IMAGE_BYTES (1992u * 1024u)

bool ota_manifest_parse(const char *body, size_t len, ota_manifest_t *out);
bool ota_is_newer(uint32_t latest, uint32_t running);
#endif
