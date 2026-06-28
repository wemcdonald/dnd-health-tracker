#include "ota_manifest.h"
#include <string.h>
#include <stdio.h>

static bool is_hex64(const char *s) {
    for (int i = 0; i < 64; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return s[64] == '\0' || s[64] == '\n' || s[64] == '\r';
}

bool ota_manifest_parse(const char *body, size_t len, ota_manifest_t *out) {
    if (len == 0 || len > 1024) return false;
    char tmp[1025];
    memcpy(tmp, body, len);
    tmp[len] = '\0';

    char *l1 = tmp;
    char *l2 = strchr(l1, '\n'); if (!l2) return false; *l2++ = '\0';
    char *l3 = strchr(l2, '\n'); if (!l3) return false; *l3++ = '\0';
    char *l3end = strpbrk(l3, "\r\n"); if (l3end) *l3end = '\0';

    unsigned ver = 0, sz = 0;
    if (sscanf(l1, "%u %u", &ver, &sz) != 2) return false;
    if (sz == 0) return false;
    if (!is_hex64(l2)) return false;
    if (l3[0] != '/') return false;
    if (strlen(l3) >= sizeof(out->path)) return false;

    out->version = ver;
    out->size = sz;
    memcpy(out->sha256, l2, 64); out->sha256[64] = '\0';
    strncpy(out->path, l3, sizeof(out->path) - 1);
    out->path[sizeof(out->path) - 1] = '\0';
    return true;
}

bool ota_is_newer(uint32_t latest, uint32_t running) {
    return latest > running;
}
