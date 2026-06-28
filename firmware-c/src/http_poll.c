/**
 * http_poll.c — minimal plain-HTTP/1.1 GET client (M4). See http_poll.h.
 *
 * Reconnects each call (Connection: close) — at a ~2s cadence the handshake
 * cost is negligible and this avoids stale-socket handling. All lwIP raw-API
 * calls are bracketed with cyw43_arch_lwip_begin/end (no-ops under poll, needed
 * under threadsafe_background since they run from core0 main context).
 */
#include "http_poll.h"

#include <string.h>
#include <stdio.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/watchdog.h"
#include "lwip/tcp.h"
#include "lwip/dns.h"

/* Small binary-safe substring search (memmem is not portably available in the
 * embedded libc). Used by the streaming parser to find "\r\n\r\n" and " 200"
 * without relying on NUL termination of the network buffer. */
static const void *memmem_local(const void *hay, size_t hlen,
                                const void *needle, size_t nlen) {
    if (nlen == 0) return hay;
    if (hlen < nlen) return NULL;
    const uint8_t *h = (const uint8_t *)hay;
    const uint8_t *n = (const uint8_t *)needle;
    for (size_t i = 0; i + nlen <= hlen; i++) {
        if (h[i] == n[0] && memcmp(h + i, n, nlen) == 0) return h + i;
    }
    return NULL;
}

/* ── DNS resolve ──────────────────────────────────────────────────────────── */

typedef struct {
    volatile bool done;
    bool          ok;
    ip_addr_t     addr;
} resolve_ctx_t;

static void dns_cb(const char *name, const ip_addr_t *ipaddr, void *arg) {
    (void)name;
    resolve_ctx_t *c = (resolve_ctx_t *)arg;
    if (ipaddr) { c->addr = *ipaddr; c->ok = true; }
    c->done = true;
}

bool http_resolve(const char *host, ip_addr_t *out, uint32_t timeout_ms) {
    /* IP literal? */
    if (ip4addr_aton(host, ip_2_ip4(out))) {
        IP_SET_TYPE(out, IPADDR_TYPE_V4);
        return true;
    }
    resolve_ctx_t ctx = {0};
    cyw43_arch_lwip_begin();
    err_t e = dns_gethostbyname(host, &ctx.addr, dns_cb, &ctx);
    cyw43_arch_lwip_end();
    if (e == ERR_OK) { *out = ctx.addr; return true; }      /* cached */
    if (e != ERR_INPROGRESS) return false;

    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    while (!ctx.done && absolute_time_diff_us(get_absolute_time(), deadline) > 0) {
        watchdog_update();
        cyw43_arch_poll();
        sleep_ms(5);
    }
    if (ctx.ok) { *out = ctx.addr; return true; }
    return false;
}

/* ── HTTP GET ─────────────────────────────────────────────────────────────── */

typedef struct {
    volatile bool   done;
    bool            pcb_freed;   /* lwIP freed the pcb (err cb) */
    struct tcp_pcb *pcb;
    char            buf[1024];
    int             len;
    const char     *request;
    int             req_len;
} get_ctx_t;

static err_t get_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    get_ctx_t *c = (get_ctx_t *)arg;
    if (err != ERR_OK) { c->done = true; return err; }
    if (!p) { c->done = true; return ERR_OK; }   /* remote closed: response complete */
    int space = (int)sizeof(c->buf) - 1 - c->len;
    int n = (int)p->tot_len;
    if (n > space) n = space;
    if (n > 0) {
        pbuf_copy_partial(p, c->buf + c->len, (u16_t)n, 0);
        c->len += n;
        c->buf[c->len] = '\0';
    }
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static err_t get_connected(void *arg, struct tcp_pcb *pcb, err_t err) {
    get_ctx_t *c = (get_ctx_t *)arg;
    if (err != ERR_OK) { c->done = true; return err; }
    tcp_write(pcb, c->request, (u16_t)c->req_len, TCP_WRITE_FLAG_COPY);
    tcp_output(pcb);
    return ERR_OK;
}

static void get_err(void *arg, err_t err) {
    (void)err;
    get_ctx_t *c = (get_ctx_t *)arg;
    c->pcb_freed = true;   /* pcb is already freed by lwIP */
    c->done = true;
}

bool http_poll_once(const ip_addr_t *ip, uint16_t port, const char *host,
                    const char *path, int *cur, int *max, int *temp, int *age_s,
                    uint32_t timeout_ms) {
    get_ctx_t ctx = {0};
    char req[256];
    ctx.req_len = snprintf(req, sizeof(req),
                           "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
                           path, host);
    ctx.request = req;

    cyw43_arch_lwip_begin();
    ctx.pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (ctx.pcb) {
        tcp_arg(ctx.pcb, &ctx);
        tcp_recv(ctx.pcb, get_recv);
        tcp_err(ctx.pcb, get_err);
    }
    err_t ce = ctx.pcb ? tcp_connect(ctx.pcb, ip, port, get_connected) : ERR_MEM;
    cyw43_arch_lwip_end();
    if (!ctx.pcb || ce != ERR_OK) {
        if (ctx.pcb && !ctx.pcb_freed) {
            cyw43_arch_lwip_begin(); tcp_abort(ctx.pcb); cyw43_arch_lwip_end();
        }
        return false;
    }

    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    while (!ctx.done && absolute_time_diff_us(get_absolute_time(), deadline) > 0) {
        watchdog_update();
        cyw43_arch_poll();
        sleep_ms(5);
    }

    /* Tear down the pcb unless lwIP already freed it via the error callback. */
    if (!ctx.pcb_freed) {
        cyw43_arch_lwip_begin();
        tcp_arg(ctx.pcb, NULL);
        tcp_recv(ctx.pcb, NULL);
        tcp_err(ctx.pcb, NULL);
        if (tcp_close(ctx.pcb) != ERR_OK) tcp_abort(ctx.pcb);
        cyw43_arch_lwip_end();
    }

    if (ctx.len <= 0) return false;

    /* Require a 200 status. */
    if (strncmp(ctx.buf, "HTTP/1.", 7) != 0 || strstr(ctx.buf, " 200") == NULL) return false;

    /* Body starts after the header terminator. */
    char *body = strstr(ctx.buf, "\r\n\r\n");
    if (!body) return false;
    body += 4;

    int c = 0, m = 0, t = 0, a = 0;
    if (sscanf(body, "%d %d %d %d", &c, &m, &t, &a) != 4) return false;
    if (m < 1) return false;            /* a live character has >= 1 max HP */
    if (c < 0) c = 0;
    if (c > m) c = m;
    *cur = c;
    *max = m;
    *temp = t;
    *age_s = a;
    return true;
}

/* ── http_get_body ────────────────────────────────────────────────────────────
 * Same raw-TCP flow as http_poll_once, but returns the post-"\r\n\r\n" body in
 * `out` (length-limited, NUL-terminated) instead of parsing four ints. Reuses
 * the 1 KB get_ctx_t buffer; suitable for small responses (e.g. the manifest).
 */
int http_get_body(const ip_addr_t *ip, uint16_t port, const char *host,
                  const char *path, char *out, size_t out_cap, uint32_t timeout_ms) {
    if (out_cap == 0) return -1;
    out[0] = '\0';

    get_ctx_t ctx = {0};
    char req[256];
    ctx.req_len = snprintf(req, sizeof(req),
                           "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
                           path, host);
    ctx.request = req;

    cyw43_arch_lwip_begin();
    ctx.pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (ctx.pcb) {
        tcp_arg(ctx.pcb, &ctx);
        tcp_recv(ctx.pcb, get_recv);
        tcp_err(ctx.pcb, get_err);
    }
    err_t ce = ctx.pcb ? tcp_connect(ctx.pcb, ip, port, get_connected) : ERR_MEM;
    cyw43_arch_lwip_end();
    if (!ctx.pcb || ce != ERR_OK) {
        if (ctx.pcb && !ctx.pcb_freed) {
            cyw43_arch_lwip_begin(); tcp_abort(ctx.pcb); cyw43_arch_lwip_end();
        }
        return -1;
    }

    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    while (!ctx.done && absolute_time_diff_us(get_absolute_time(), deadline) > 0) {
        watchdog_update();
        cyw43_arch_poll();
        sleep_ms(5);
    }

    if (!ctx.pcb_freed) {
        cyw43_arch_lwip_begin();
        tcp_arg(ctx.pcb, NULL);
        tcp_recv(ctx.pcb, NULL);
        tcp_err(ctx.pcb, NULL);
        if (tcp_close(ctx.pcb) != ERR_OK) tcp_abort(ctx.pcb);
        cyw43_arch_lwip_end();
    }

    if (ctx.len <= 0) return -1;
    if (strncmp(ctx.buf, "HTTP/1.", 7) != 0 || strstr(ctx.buf, " 200") == NULL) return -1;

    char *body = strstr(ctx.buf, "\r\n\r\n");
    if (!body) return -1;
    body += 4;

    size_t blen = (size_t)(ctx.buf + ctx.len - body);
    if (blen > out_cap - 1) blen = out_cap - 1;
    memcpy(out, body, blen);
    out[blen] = '\0';
    return (int)blen;
}

/* ── http_get_stream ──────────────────────────────────────────────────────────
 * Streams a GET response body to a sink callback without ever buffering the whole
 * image. State machine over the incoming pbuf stream:
 *
 *   PHASE 1 (headers): accumulate received bytes into a small bounded header
 *     buffer until the "\r\n\r\n" terminator appears. The terminator may straddle
 *     a pbuf boundary, so we scan the accumulated buffer (which retains up to the
 *     last 3 bytes of the prior chunk for exactly this reason). Bail if the headers
 *     exceed the buffer (malformed / hostile server) or the status line is not
 *     "HTTP/1." + " 200".
 *   PHASE 2 (body): every byte after the terminator — including the body bytes that
 *     arrived in the SAME pbuf as the end of the headers — is forwarded to sink().
 *     Subsequent pbufs are forwarded straight through, no buffering.
 *
 * Backpressure: we call tcp_recved() only for bytes we have actually consumed
 * (header bytes scanned + body bytes the sink accepted). If sink() returns false
 * we abort the connection and report failure.
 *
 * Body end: the server sends "Connection: close", so a clean remote close (p==NULL
 * in the recv cb) after >0 forwarded body bytes is treated as success. We do not
 * parse Content-Length — close-delimited is the robust choice here.
 */

#define STREAM_HDR_MAX 1024     /* fail if headers exceed this */

typedef struct {
    volatile bool   done;
    bool            pcb_freed;
    bool            ok;          /* set true once we have a 200 + header terminator */
    bool            failed;      /* sticky: bad status / oversized headers / sink reject */
    struct tcp_pcb *pcb;

    /* Header-accumulation buffer, used only during PHASE 1. */
    char            hdr[STREAM_HDR_MAX + 1];
    int             hdr_len;
    bool            in_body;     /* true once "\r\n\r\n" has been crossed */

    int             body_total;  /* bytes forwarded to sink */

    bool          (*sink)(void *ctx, const uint8_t *data, size_t len);
    void           *sink_ctx;

    const char     *request;
    int             req_len;
} stream_ctx_t;

/* Forward `len` bytes of body to the sink; update accounting / failure state.
 * Returns the number of bytes the caller may tcp_recved() for this call (i.e. the
 * bytes we have taken responsibility for). On sink failure marks failed and
 * returns the count anyway (the bytes left the pbuf; the connection will abort). */
static int stream_emit(stream_ctx_t *c, const uint8_t *data, size_t len) {
    if (len == 0) return 0;
    if (!c->sink(c->sink_ctx, data, len)) {
        c->failed = true;
        return (int)len;   /* consumed from the pbuf even though we now abort */
    }
    c->body_total += (int)len;
    return (int)len;
}

static err_t stream_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    stream_ctx_t *c = (stream_ctx_t *)arg;
    if (err != ERR_OK) { c->failed = true; c->done = true; if (p) pbuf_free(p); return err; }
    if (!p) { c->done = true; return ERR_OK; }   /* remote closed: end of body */

    /* Walk the pbuf chain segment by segment so a chunk that spans multiple
     * pbufs in the chain is handled without an intermediate copy of the body. */
    u16_t consumed = 0;
    for (struct pbuf *q = p; q != NULL; q = q->next) {
        const uint8_t *seg = (const uint8_t *)q->payload;
        u16_t seg_len = q->len;
        u16_t seg_off = 0;

        if (!c->in_body) {
            /* PHASE 1: append to the bounded header buffer, then look for the
             * terminator. The buffer retains the running headers, so a "\r\n\r\n"
             * split across pbufs is found here. */
            int space = STREAM_HDR_MAX - c->hdr_len;
            int take = seg_len < space ? seg_len : space;
            if (take > 0) {
                memcpy(c->hdr + c->hdr_len, seg, (size_t)take);
                c->hdr_len += take;
                c->hdr[c->hdr_len] = '\0';
            }
            char *term = (char *)memmem_local(c->hdr, (size_t)c->hdr_len, "\r\n\r\n", 4);
            if (!term) {
                /* No terminator yet. If the buffer is full and still no terminator,
                 * the headers are too large — fail. Otherwise we have consumed this
                 * whole segment into the header buffer. */
                if (c->hdr_len >= STREAM_HDR_MAX) { c->failed = true; c->done = true; break; }
                consumed += seg_len;
                continue;
            }
            /* Terminator found. Validate the status line now (once). */
            if (strncmp(c->hdr, "HTTP/1.", 7) != 0 ||
                memmem_local(c->hdr, (size_t)c->hdr_len, " 200", 4) == NULL) {
                c->failed = true; c->done = true; break;
            }
            c->ok = true;
            c->in_body = true;

            /* Body bytes that arrived in THIS segment start after the terminator.
             * Compute how many header bytes from THIS segment we appended (`take`),
             * and how many of those were body (past the terminator within hdr). */
            int hdr_bytes = (int)((term + 4) - c->hdr);    /* header length incl. terminator */
            int body_in_hdr = c->hdr_len - hdr_bytes;       /* leftover body sitting in hdr buf */
            /* Of `take` bytes appended from this segment, the trailing `body_in_hdr`
             * are body (the terminator could also have been wholly in a PRIOR segment,
             * in which case body_in_hdr may exceed `take` — clamp to this segment). */
            int body_from_seg = body_in_hdr < take ? body_in_hdr : take;
            if (body_from_seg < 0) body_from_seg = 0;
            /* The body bytes within this segment begin at offset (take - body_from_seg). */
            seg_off = (u16_t)(take - body_from_seg);
            /* Account the header bytes of this segment as consumed up front. */
            consumed += seg_off;
            /* Fall through to emit the body remainder of this segment below. */
        }

        /* PHASE 2: forward the body portion of this segment. */
        if (c->in_body && !c->failed) {
            u16_t blen = (u16_t)(seg_len - seg_off);
            int e = stream_emit(c, seg + seg_off, (size_t)blen);
            consumed += (u16_t)e;
            if (c->failed) break;
        }
    }

    if (consumed) tcp_recved(pcb, consumed);
    pbuf_free(p);
    if (c->failed) c->done = true;
    return ERR_OK;
}

static err_t stream_connected(void *arg, struct tcp_pcb *pcb, err_t err) {
    stream_ctx_t *c = (stream_ctx_t *)arg;
    if (err != ERR_OK) { c->failed = true; c->done = true; return err; }
    tcp_write(pcb, c->request, (u16_t)c->req_len, TCP_WRITE_FLAG_COPY);
    tcp_output(pcb);
    return ERR_OK;
}

static void stream_err(void *arg, err_t err) {
    (void)err;
    stream_ctx_t *c = (stream_ctx_t *)arg;
    c->pcb_freed = true;
    c->failed = true;
    c->done = true;
}

int http_get_stream(const ip_addr_t *ip, uint16_t port, const char *host, const char *path,
                    bool (*sink)(void *ctx, const uint8_t *data, size_t len), void *ctx,
                    uint32_t timeout_ms) {
    static stream_ctx_t sc;   /* ~1 KB header buffer; keep off the caller's stack */
    memset(&sc, 0, sizeof(sc));
    sc.sink = sink;
    sc.sink_ctx = ctx;

    char req[256];
    sc.req_len = snprintf(req, sizeof(req),
                          "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
                          path, host);
    sc.request = req;

    cyw43_arch_lwip_begin();
    sc.pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (sc.pcb) {
        tcp_arg(sc.pcb, &sc);
        tcp_recv(sc.pcb, stream_recv);
        tcp_err(sc.pcb, stream_err);
    }
    err_t ce = sc.pcb ? tcp_connect(sc.pcb, ip, port, stream_connected) : ERR_MEM;
    cyw43_arch_lwip_end();
    if (!sc.pcb || ce != ERR_OK) {
        if (sc.pcb && !sc.pcb_freed) {
            cyw43_arch_lwip_begin(); tcp_abort(sc.pcb); cyw43_arch_lwip_end();
        }
        return -1;
    }

    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    while (!sc.done && absolute_time_diff_us(get_absolute_time(), deadline) > 0) {
        watchdog_update();
        cyw43_arch_poll();
        sleep_ms(5);
    }

    bool timed_out = !sc.done;

    /* Tear down. On sink-reject / bad-status / timeout we abort (drops the half
     * stream); on a clean finish we close. lwIP may already have freed the pcb. */
    if (!sc.pcb_freed) {
        cyw43_arch_lwip_begin();
        tcp_arg(sc.pcb, NULL);
        tcp_recv(sc.pcb, NULL);
        tcp_err(sc.pcb, NULL);
        if (sc.failed || timed_out) {
            tcp_abort(sc.pcb);
        } else if (tcp_close(sc.pcb) != ERR_OK) {
            tcp_abort(sc.pcb);
        }
        cyw43_arch_lwip_end();
    }

    if (sc.failed || timed_out) return -1;
    if (!sc.ok || sc.body_total <= 0) return -1;   /* never saw 200/body */
    return sc.body_total;
}
