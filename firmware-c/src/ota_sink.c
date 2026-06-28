#include "ota_sink.h"
#include <string.h>

void ota_sink_init(ota_sink_t *s, ota_sector_write_fn write, void *ctx) {
    s->write = write; s->ctx = ctx;
    s->fill = 0; s->sector_index = 0; s->failed = false;
}

static bool flush_full(ota_sink_t *s) {
    if (!s->write(s->ctx, s->sector_index, s->buf)) { s->failed = true; return false; }
    s->sector_index++;
    s->fill = 0;
    return true;
}

bool ota_sink_push(ota_sink_t *s, const uint8_t *data, size_t len) {
    if (s->failed) return false;
    while (len > 0) {
        size_t space = OTA_SECTOR - s->fill;
        size_t n = len < space ? len : space;
        memcpy(s->buf + s->fill, data, n);
        s->fill += n; data += n; len -= n;
        if (s->fill == OTA_SECTOR && !flush_full(s)) return false;
    }
    return true;
}

bool ota_sink_finish(ota_sink_t *s) {
    if (s->failed) return false;
    if (s->fill == 0) return true;
    memset(s->buf + s->fill, 0xFF, OTA_SECTOR - s->fill);
    return flush_full(s);
}
