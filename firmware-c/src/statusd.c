/**
 * statusd.c — STA-mode web server (status + editable config) + mDNS responder.
 *
 * The page itself (status table + setup form, pre-filled) and the save logic are
 * shared with the AP portal in configform.c. This file is just the mDNS
 * responder + the TCP server + request routing.
 */
#include "statusd.h"
#include "configform.h"
#include "boot_mode.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

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
    char request[2560];   // GET line + headers (query can hold up to MAX_NETS networks)
    int  req_len;
    bool handled;
    char resp[4096];
    int  resp_len;
    int  sent_len;
    bool reboot_after;
    bool reboot_to_portal;
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
        bool reboot = c->reboot_after, to_portal = c->reboot_to_portal;
        free(c);
        if (reboot) {
            if (to_portal) watchdog_hw->scratch[0] = PORTAL_FORCE_MAGIC;
            watchdog_reboot(0, 0, 300);
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

static err_t poll_cb(void *arg, struct tcp_pcb *pcb) { return conn_close((conn_t *)arg, pcb); }

static void err_cb(void *arg, err_t err) { (void)err; if (arg) free(arg); }

static err_t recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    conn_t *c = (conn_t *)arg;
    if (!p || err != ERR_OK) return conn_close(c, pcb);

    int space = (int)sizeof(c->request) - 1 - c->req_len;
    int n = (int)p->tot_len;
    if (n > space) n = space;
    if (n > 0) {
        pbuf_copy_partial(p, c->request + c->req_len, (u16_t)n, 0);
        c->req_len += n;
        c->request[c->req_len] = '\0';
    }
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);

    if (c->handled) return ERR_OK;
    if (!strstr(c->request, "\r\n\r\n")) return ERR_OK;  /* await full request */
    c->handled = true;

    if (strncmp(c->request, "GET /save?", 10) == 0) {
        char *q = c->request + 10;
        char *end = strstr(q, " HTTP");
        if (end) *end = '\0';
        bool ok = configform_save(q);
        c->resp_len = ok
            ? configform_simple(c->resp, sizeof(c->resp), "Saved &amp; rebooting...",
                                "Reconnecting to WiFi. This page returns shortly.")
            : configform_simple(c->resp, sizeof(c->resp), "Save failed",
                                "At least one network is required. Go back and retry.");
        c->reboot_after = ok;
    } else if (strncmp(c->request, "POST /reconfigure", 17) == 0 ||
               strncmp(c->request, "GET /reconfigure", 16) == 0) {
        c->resp_len = configform_simple(c->resp, sizeof(c->resp), "Rebooting into AP setup...",
                                        "Join the <b>healthbar-setup</b> network shortly.");
        c->reboot_after = true;
        c->reboot_to_portal = true;
    } else {
        c->resp_len = configform_page(c->resp, sizeof(c->resp), true, s_slug, s_poll_desc);
    }
    c->sent_len = 0;

    err_t w = tcp_write(pcb, c->resp, (u16_t)c->resp_len, TCP_WRITE_FLAG_COPY);
    if (w == ERR_OK) w = tcp_output(pcb);
    if (w != ERR_OK) {
        c->reboot_after = false;  /* don't reboot on a failed send */
        return conn_close(c, pcb);
    }
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
    tcp_poll(pcb, poll_cb, 8);
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
