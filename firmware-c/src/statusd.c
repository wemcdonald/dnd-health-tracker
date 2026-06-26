/**
 * statusd.c — STA-mode status web server + mDNS responder. See statusd.h.
 *
 * Mirrors the portal's raw-TCP server pattern, but serves one dynamic status
 * page and a POST /reconfigure that reboots into the setup portal. lwIP raw-API
 * calls outside callbacks are bracketed with cyw43_arch_lwip_begin/end.
 */
#include "statusd.h"
#include "health.h"
#include "boot_mode.h"

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/watchdog.h"
#include "hardware/structs/watchdog.h"

#include "lwip/tcp.h"
#include "lwip/netif.h"
#include "lwip/apps/mdns.h"

#define STATUS_PORT 80

static char s_slug[48];
static char s_poll_desc[96];
static char s_hostname[56];   // "healthbar-<slug>"

typedef struct {
    char request[512];
    char resp[1400];
    int  resp_len;
    int  sent_len;
    bool reboot_after;
} conn_t;

/* ── mDNS ─────────────────────────────────────────────────────────────────── */

static void srv_txt(struct mdns_service *service, void *txt_userdata) {
    (void)txt_userdata;
    mdns_resp_add_service_txtitem(service, "path=/", 6);
}

static void mdns_start(void) {
    struct netif *nif = &cyw43_state.netif[CYW43_ITF_STA];
    cyw43_arch_lwip_begin();
    mdns_resp_init();
    mdns_resp_add_netif(nif, s_hostname);
    mdns_resp_add_service(nif, s_hostname, "_http", DNSSD_PROTO_TCP, STATUS_PORT, srv_txt, NULL);
    cyw43_arch_lwip_end();
}

/* ── status page ──────────────────────────────────────────────────────────── */

static const char *state_str(anim_state_t st) {
    switch (st) {
        case ST_BOOT:       return "boot";
        case ST_CONNECTING: return "connecting";
        case ST_LIVE:       return "live";
        case ST_OFFLINE:    return "offline";
        default:            return "?";
    }
}

static int build_status_page(char *out, int cap) {
    health_t h = health_snapshot();
    uint32_t up = to_ms_since_boot(get_absolute_time()) / 1000;
    const char *ip = ip4addr_ntoa(netif_ip4_addr(&cyw43_state.netif[CYW43_ITF_STA]));

    return snprintf(out, cap,
        "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
        "Connection: close\r\n\r\n"
        "<!DOCTYPE html><html><head><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<meta http-equiv=refresh content=2>"
        "<title>healthbar %s</title><style>"
        "body{font-family:sans-serif;margin:24px auto;max-width:380px;padding:0 14px;color:#222}"
        "h2{margin:.2em 0}table{border-collapse:collapse;width:100%%;margin:1em 0}"
        "td{padding:.35rem .5rem;border-bottom:1px solid #eee}td:first-child{color:#777;width:9em}"
        "button{width:100%%;padding:12px;font-size:15px;margin-top:1em}"
        "</style></head><body>"
        "<h2>healthbar: %s</h2>"
        "<table>"
        "<tr><td>state</td><td><b>%s</b></td></tr>"
        "<tr><td>LEDs lit</td><td>%d / %d</td></tr>"
        "<tr><td>upstream age</td><td>%d s</td></tr>"
        "<tr><td>IP</td><td>%s</td></tr>"
        "<tr><td>polling</td><td>%s</td></tr>"
        "<tr><td>uptime</td><td>%lu s</td></tr>"
        "</table>"
        "<form method=POST action=/reconfigure "
        "onsubmit=\"return confirm('Reboot into WiFi setup?')\">"
        "<button>Reconfigure WiFi (reboot to setup)</button></form>"
        "</body></html>",
        s_slug, s_slug, state_str(h.state), h.lit, NUM_LEDS, h.age_s,
        ip, s_poll_desc, (unsigned long)up);
}

static int build_reboot_page(char *out, int cap) {
    return snprintf(out, cap,
        "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
        "Connection: close\r\n\r\n"
        "<!DOCTYPE html><html><body><h2>Rebooting into WiFi setup...</h2>"
        "<p>Join the <b>healthbar-setup-%s</b> network shortly.</p></body></html>",
        s_slug);
}

/* ── TCP server ───────────────────────────────────────────────────────────── */

static err_t conn_close(conn_t *c, struct tcp_pcb *pcb) {
    if (pcb) {
        tcp_arg(pcb, NULL);
        tcp_sent(pcb, NULL);
        tcp_recv(pcb, NULL);
        tcp_err(pcb, NULL);
        tcp_poll(pcb, NULL, 0);
        if (tcp_close(pcb) != ERR_OK) tcp_abort(pcb);
    }
    if (c) {
        bool reboot = c->reboot_after;
        free(c);
        if (reboot) {
            watchdog_hw->scratch[0] = PORTAL_FORCE_MAGIC;
            watchdog_reboot(0, 0, 200);
        }
    }
    return ERR_OK;
}

static err_t sent_cb(void *arg, struct tcp_pcb *pcb, u16_t len) {
    conn_t *c = (conn_t *)arg;
    c->sent_len += len;
    if (c->sent_len >= c->resp_len) return conn_close(c, pcb);
    return ERR_OK;
}

static err_t poll_cb(void *arg, struct tcp_pcb *pcb) {
    return conn_close((conn_t *)arg, pcb);
}

static void err_cb(void *arg, err_t err) {
    (void)err;
    if (arg) free(arg);   /* pcb already freed by lwIP */
}

static err_t recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    conn_t *c = (conn_t *)arg;
    if (!p || err != ERR_OK) return conn_close(c, pcb);

    size_t n = p->tot_len;
    if (n > sizeof(c->request) - 1) n = sizeof(c->request) - 1;
    pbuf_copy_partial(p, c->request, (u16_t)n, 0);
    c->request[n] = '\0';
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);

    bool reconfigure = (strncmp(c->request, "POST /reconfigure", 17) == 0);
    if (reconfigure) {
        c->resp_len = build_reboot_page(c->resp, sizeof(c->resp));
        c->reboot_after = true;
    } else {
        c->resp_len = build_status_page(c->resp, sizeof(c->resp));
    }
    c->sent_len = 0;

    tcp_write(pcb, c->resp, (u16_t)c->resp_len, TCP_WRITE_FLAG_COPY);
    tcp_output(pcb);
    return ERR_OK;
}

static err_t accept_cb(void *arg, struct tcp_pcb *pcb, err_t err) {
    (void)arg;
    if (err != ERR_OK || !pcb) return ERR_VAL;
    conn_t *c = (conn_t *)calloc(1, sizeof(conn_t));
    if (!c) { tcp_abort(pcb); return ERR_ABRT; }
    tcp_arg(pcb, c);
    tcp_recv(pcb, recv_cb);
    tcp_sent(pcb, sent_cb);
    tcp_poll(pcb, poll_cb, 8);   /* close idle conns (~4s) */
    tcp_err(pcb, err_cb);
    return ERR_OK;
}

void statusd_start(const char *slug, const char *poll_desc) {
    strncpy(s_slug, slug, sizeof(s_slug) - 1);
    strncpy(s_poll_desc, poll_desc, sizeof(s_poll_desc) - 1);
    snprintf(s_hostname, sizeof(s_hostname), "healthbar-%s", s_slug);

    mdns_start();

    cyw43_arch_lwip_begin();
    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (pcb && tcp_bind(pcb, IP_ANY_TYPE, STATUS_PORT) == ERR_OK) {
        pcb = tcp_listen_with_backlog(pcb, 2);
        tcp_accept(pcb, accept_cb);
    }
    cyw43_arch_lwip_end();

    printf("statusd: http://%s.local/ (and the device IP) on port %d\n", s_hostname, STATUS_PORT);
}
