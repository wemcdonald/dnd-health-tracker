/**
 * http_poll.h — minimal plain-HTTP/1.1 GET client (M4).
 *
 * Polls the server's precomputed file and parses line 1
 * ("<cur> <max> <temp> <age>"). No TLS. Uses the lwIP raw TCP API under
 * cyw43_arch threadsafe_background.
 */
#ifndef HEALTHBAR_HTTP_POLL_H
#define HEALTHBAR_HTTP_POLL_H

#include <stdbool.h>
#include <stdint.h>
#include "lwip/ip_addr.h"

/* Resolve an IP literal or DNS hostname. Blocks (pumping cyw43) up to
 * timeout_ms for DNS. Returns true and fills *out on success. */
bool http_resolve(const char *host, ip_addr_t *out, uint32_t timeout_ms);

/* One HTTP/1.1 GET of `path` from ip:port (Host: host). On a 200 response whose
 * body line 1 parses as four ints, returns true and sets *cur/*max/*temp/*age_s
 * (cur clamped to 0..max, max forced >= 1). Any failure (connect/timeout/non-
 * 200/parse) returns false. */
bool http_poll_once(const ip_addr_t *ip, uint16_t port, const char *host,
                    const char *path, int *cur, int *max, int *temp, int *age_s,
                    uint32_t timeout_ms);

/* Generic GET: copies response body (after headers) into out (<= out_cap-1,
 * NUL-terminated). Returns body length, or -1 on failure (connect/timeout/
 * non-200). Body is delimited by the remote close (Connection: close). */
int http_get_body(const ip_addr_t *ip, uint16_t port, const char *host,
                  const char *path, char *out, size_t out_cap, uint32_t timeout_ms);

/* Streamed GET: after skipping headers, invokes sink(ctx, chunk, len) for each
 * body chunk as it arrives (no whole-image buffering). Returns total body bytes
 * forwarded, or -1 on failure. If sink returns false, the transfer aborts and
 * returns -1. */
int http_get_stream(const ip_addr_t *ip, uint16_t port, const char *host, const char *path,
                    bool (*sink)(void *ctx, const uint8_t *data, size_t len), void *ctx,
                    uint32_t timeout_ms);

#endif /* HEALTHBAR_HTTP_POLL_H */
