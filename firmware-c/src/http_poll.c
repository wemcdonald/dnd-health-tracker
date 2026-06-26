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
#include "lwip/tcp.h"
#include "lwip/dns.h"

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
                    const char *path, int *lit, int *age_s, uint32_t timeout_ms) {
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

    int l = 0, a = 0;
    if (sscanf(body, "%d %d", &l, &a) != 2) return false;
    if (l < 0 || l > 16) return false;
    *lit = l;
    *age_s = a;
    return true;
}
