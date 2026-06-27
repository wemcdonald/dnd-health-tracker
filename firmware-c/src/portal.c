/**
 * portal.c — M1 milestone: captive-portal AP mode
 *
 * Raises an open AP named "healthbar-setup-<name>", where <name> is set at
 * build time via -DHEALTHBAR_NAME=shen (so you know which bar is which).
 * Runs DHCP + DNS catch-all so any HTTP request from a phone triggers the
 * "Sign in" sheet and lands on our setup form.
 *
 * Design constraints (from design doc / cold-boot-hang investigation):
 *   - pico_cyw43_arch_lwip_threadsafe_background: all cyw43/lwIP work on core0.
 *   - ONE cyw43_arch_init per boot. No STA scan here — that is the cold-boot
 *     hang trigger (STA scan while AP active on freshly-loaded cyw43).
 *   - AP-only epoch. STA connect lives in a separate reboot epoch (M3+).
 *   - lwIP raw API calls bracketed with cyw43_arch_lwip_begin/end.
 *
 * On form submit: parses the values, persists them to flash (config_save),
 * returns a "Saved & rebooting" page, then reboots into STA mode.
 *
 * HTML form: <1.5 KB so it fits in one lwIP TCP segment (TCP_MSS=1460).
 *
 * SPDX-License-Identifier: BSD-3-Clause (portions from pico-examples)
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/watchdog.h"
#include "hardware/structs/watchdog.h"

#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "lwip/netif.h"

#include "dhcpserver.h"
#include "dnsserver.h"
#include "config.h"
#include "http_poll.h"
#include "health.h"
#include "leds.h"
#include "statusd.h"
#include "boot_mode.h"
#include "provision.h"
#include "configform.h"

/* ── Constants ────────────────────────────────────────────────────────────── */

#define AP_IP_ADDR  "192.168.4.1"
#define TCP_PORT    80
/* MAX_NETS comes from config.h */

/* PORTAL_FORCE_MAGIC (watchdog-scratch flag) is defined in boot_mode.h, shared
 * with the status server's "Reconfigure" button. */

/* Setup-AP suffix, configured at build time: -DHEALTHBAR_NAME=shen.
 * Empty -> SSID is just "healthbar-setup". */
#ifndef HEALTHBAR_NAME
#define HEALTHBAR_NAME ""
#endif

/* Poll target (overridable at build time, e.g. -DPOLL_HOST=10.0.10.123 -DPOLL_PORT=8080). */
#ifndef POLL_HOST
#define POLL_HOST "dndhealth.willflix.org"
#endif
#ifndef POLL_PORT
#define POLL_PORT 80
#endif

/* How long to keep an idle HTTP connection open before closing it. */
#define HTTP_POLL_INTERVAL_S 5

/* ── HTML pages ───────────────────────────────────────────────────────────── */
/* The setup form + save logic live in configform.c (shared with the STA status
 * page). Only the captive-portal catch-all redirect is portal-specific. */

/* Captive-portal catch-all: 200 + meta-refresh. iOS/Android pop "Sign in"
 * more reliably on 200 than on 302. */
static const char CATCHALL_PAGE[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/html; charset=utf-8\r\n"
    "Connection: close\r\n"
    "\r\n"
    "<!DOCTYPE html><html><head>"
    "<meta http-equiv=refresh content='0; url=http://" AP_IP_ADDR "/'>"
    "</head><body>Redirecting...</body></html>";

/* ── TCP server state ─────────────────────────────────────────────────────── */

typedef struct {
    struct tcp_pcb *server_pcb;
    bool reboot_pending;
    ip_addr_t gw;
} portal_state_t;

typedef struct {
    struct tcp_pcb *pcb;
    int sent_len;
    char request_buf[2560]; /* GET line + headers (a 5-network save query is large) */
    int  req_len;
    bool handled;
    char resp[2600];        /* rendered config page (configform_page) */
    const char *response;   /* -> resp, or a static page like CATCHALL_PAGE */
    int response_len;
    portal_state_t *portal;
} conn_state_t;

/* ── TCP connection helpers (mirrored from pico-examples access_point) ────── */

static err_t conn_close(conn_state_t *cs, struct tcp_pcb *pcb, err_t close_err) {
    if (pcb) {
        tcp_arg(pcb, NULL);
        tcp_poll(pcb, NULL, 0);
        tcp_sent(pcb, NULL);
        tcp_recv(pcb, NULL);
        tcp_err(pcb, NULL);
        err_t err = tcp_close(pcb);
        if (err != ERR_OK) {
            tcp_abort(pcb);
            close_err = ERR_ABRT;
        }
        if (cs) free(cs);
    }
    return close_err;
}

static err_t tcp_sent_cb(void *arg, struct tcp_pcb *pcb, u16_t len) {
    conn_state_t *cs = (conn_state_t *)arg;
    cs->sent_len += len;
    if (cs->sent_len >= cs->response_len) {
        /* All bytes sent — close connection, then maybe reboot. */
        err_t err = conn_close(cs, pcb, ERR_OK);
        /* cs is now freed; check flag on parent state via local copy. */
        return err;
    }
    return ERR_OK;
}

static err_t tcp_poll_cb(void *arg, struct tcp_pcb *pcb) {
    conn_state_t *cs = (conn_state_t *)arg;
    if (cs) {
        return conn_close(cs, pcb, ERR_OK);
    }
    return ERR_OK;
}

static void tcp_err_cb(void *arg, err_t err) {
    conn_state_t *cs = (conn_state_t *)arg;
    if (cs && err != ERR_ABRT) {
        conn_close(cs, cs->pcb, err);
    }
}

/* ── HTTP receive callback ────────────────────────────────────────────────── */

static err_t tcp_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    conn_state_t *cs = (conn_state_t *)arg;

    if (!p) return conn_close(cs, pcb, ERR_OK);

    /* Accumulate (a 5-network /save query can span TCP segments). */
    int space = (int)sizeof(cs->request_buf) - 1 - cs->req_len;
    int n = (int)p->tot_len;
    if (n > space) n = space;
    if (n > 0) {
        pbuf_copy_partial(p, cs->request_buf + cs->req_len, (u16_t)n, 0);
        cs->req_len += n;
        cs->request_buf[cs->req_len] = '\0';
    }
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);

    if (cs->handled) return ERR_OK;
    if (strncmp(cs->request_buf, "GET ", 4) != 0) {
        /* Only GET is expected; once a line is seen and it isn't GET, drop it. */
        if (strstr(cs->request_buf, "\r\n")) return conn_close(cs, pcb, ERR_OK);
        return ERR_OK;
    }
    if (!strstr(cs->request_buf, "\r\n\r\n")) return ERR_OK;  /* await full request */
    cs->handled = true;

    /* Parse "GET <path>[?<qs>] HTTP/...". */
    char *path_start = cs->request_buf + 4;
    char *sp = strchr(path_start, ' ');
    if (sp) *sp = '\0';
    char *qs = strchr(path_start, '?');
    if (qs) *qs++ = '\0';

    if (strcmp(path_start, "/save") == 0 && qs) {
        bool ok = configform_save(qs);
        cs->response_len = ok
            ? configform_simple(cs->resp, sizeof(cs->resp), "Saved &amp; rebooting...",
                                "Connecting to WiFi. You can close this window.")
            : configform_simple(cs->resp, sizeof(cs->resp), "Save failed",
                                "At least one network is required. <a href='/'>Back</a>");
        cs->response = cs->resp;
        if (ok) cs->portal->reboot_pending = true;
    } else if (strcmp(path_start, "/") == 0 || strcmp(path_start, "/index.html") == 0) {
        cs->response_len = configform_page(cs->resp, sizeof(cs->resp), false, NULL, NULL);
        cs->response = cs->resp;
    } else {
        cs->response = CATCHALL_PAGE;
        cs->response_len = (int)strlen(CATCHALL_PAGE);
    }
    cs->sent_len = 0;

    cyw43_arch_lwip_begin();
    err_t write_err = tcp_write(pcb, cs->response, (u16_t)cs->response_len, TCP_WRITE_FLAG_COPY);
    if (write_err == ERR_OK) write_err = tcp_output(pcb);
    cyw43_arch_lwip_end();
    if (write_err != ERR_OK) return conn_close(cs, pcb, write_err);
    return ERR_OK;
}

/* ── Accept callback ──────────────────────────────────────────────────────── */

static err_t tcp_accept_cb(void *arg, struct tcp_pcb *client_pcb, err_t err) {
    portal_state_t *portal = (portal_state_t *)arg;
    if (err != ERR_OK || !client_pcb) return ERR_VAL;

    conn_state_t *cs = calloc(1, sizeof(conn_state_t));
    if (!cs) return ERR_MEM;

    cs->pcb    = client_pcb;
    cs->portal = portal;

    tcp_arg(client_pcb,  cs);
    tcp_sent(client_pcb, tcp_sent_cb);
    tcp_recv(client_pcb, tcp_recv_cb);
    tcp_poll(client_pcb, tcp_poll_cb, HTTP_POLL_INTERVAL_S * 2);
    tcp_err(client_pcb,  tcp_err_cb);

    return ERR_OK;
}

/* ── Portal init / run ────────────────────────────────────────────────────── */

static bool portal_open(portal_state_t *portal) {
    cyw43_arch_lwip_begin();
    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    cyw43_arch_lwip_end();

    if (!pcb) {
        printf("portal: failed to create PCB\n");
        return false;
    }

    cyw43_arch_lwip_begin();
    err_t err = tcp_bind(pcb, IP_ANY_TYPE, TCP_PORT);
    cyw43_arch_lwip_end();

    if (err) {
        printf("portal: tcp_bind failed %d\n", err);
        return false;
    }

    cyw43_arch_lwip_begin();
    portal->server_pcb = tcp_listen_with_backlog(pcb, 4);
    cyw43_arch_lwip_end();

    if (!portal->server_pcb) {
        printf("portal: tcp_listen failed\n");
        tcp_close(pcb);
        return false;
    }

    tcp_arg(portal->server_pcb, portal);
    tcp_accept(portal->server_pcb, tcp_accept_cb);
    return true;
}

/* ── STA mode (M3) ────────────────────────────────────────────────────────── */

/* Set the portal-force flag and reboot, so the next boot raises the setup
 * portal instead of retrying a failing STA connect forever. */
static void reboot_to_portal(void) {
    printf("STA: all networks failed — rebooting into setup portal...\n");
    watchdog_hw->scratch[0] = PORTAL_FORCE_MAGIC;
    sleep_ms(200);
    watchdog_reboot(0, 0, 100);
    while (true) tight_loop_contents();
}

/* Sleep ~ms while staying responsive: feed the watchdog, service USB-serial
 * provisioning, and pump lwIP. */
static void idle_ms(uint32_t ms) {
    absolute_time_t end = make_timeout_time_ms(ms);
    while (absolute_time_diff_us(get_absolute_time(), end) > 0) {
        watchdog_update();
        provision_poll();
        cyw43_arch_poll();
        sleep_ms(50);
    }
}

/* M4: poll the server for "<lit> <age>" every ~2s and report over serial.
 * The LED render (M5) will consume `lit`; for now we log it. Never returns. */
static void run_poll_loop(const char *slug) {
    char path[80];
    snprintf(path, sizeof(path), "/%s.txt", slug);

    ip_addr_t srv;
    bool resolved = http_resolve(POLL_HOST, &srv, 8000);
    printf("poll: target http://%s:%d%s (resolve %s)\n",
           POLL_HOST, (int)POLL_PORT, path, resolved ? "ok" : "FAILED");

    int last_lit = -1;
    while (true) {
        watchdog_update();
        if (!resolved) resolved = http_resolve(POLL_HOST, &srv, 8000);
        if (resolved) {
            int lit, age;
            if (http_poll_once(&srv, (uint16_t)POLL_PORT, POLL_HOST, path, &lit, &age, 4000)) {
                printf("poll: lit=%d age=%ds%s\n", lit, age,
                       lit != last_lit ? "  <-- changed" : "");
                health_set_lit(lit, age);   /* M5: drives the LED bar */
                last_lit = lit;
            } else {
                printf("poll: FAILED (offline)\n");
                health_set_state(ST_OFFLINE);
            }
        } else {
            /* DNS not resolving: we're connected to wifi but can't reach the
             * server. Show OFFLINE (breathing) rather than sitting forever on
             * the pre-connect CONNECTING animation. */
            printf("poll: resolve FAILED (offline)\n");
            health_set_state(ST_OFFLINE);
        }
        idle_ms(2000);  /* responsive: feeds watchdog + handles provisioning */
    }
}

/* Connect to one network using the async API, feeding the watchdog while we wait
 * (the blocking connect_timeout_ms would starve the watchdog for up to 20s). */
static bool sta_connect_one(const wifi_net_t *n, uint32_t timeout_ms) {
    bool open = (n->psk[0] == '\0');
    int rc = cyw43_arch_wifi_connect_async(
        n->ssid, open ? NULL : n->psk,
        open ? CYW43_AUTH_OPEN : CYW43_AUTH_WPA2_AES_PSK);
    if (rc != 0) return false;

    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    while (absolute_time_diff_us(get_absolute_time(), deadline) > 0) {
        watchdog_update();
        int st = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
        if (st == CYW43_LINK_UP) return true;
        if (st < 0) return false;   /* LINK_FAIL / NONET / BADAUTH */
        cyw43_arch_poll();
        sleep_ms(50);
    }
    return false;  /* timeout */
}

/* Connect to a stored network (priority order), then poll the server.
 * Never returns: either it polls forever, or it reboots into the portal on
 * total connect failure. */
static void run_sta_mode(const persist_t *cfg) {
    cyw43_arch_enable_sta_mode();
    health_set_state(ST_CONNECTING);

    /* Insertion-sort an index list by ascending priority (lower tried first). */
    int order[MAX_NETS];
    for (int i = 0; i < cfg->net_count; i++) order[i] = i;
    for (int a = 0; a < cfg->net_count; a++)
        for (int b = a + 1; b < cfg->net_count; b++)
            if (cfg->nets[order[b]].priority < cfg->nets[order[a]].priority) {
                int t = order[a]; order[a] = order[b]; order[b] = t;
            }

    for (int k = 0; k < cfg->net_count; k++) {
        const wifi_net_t *n = &cfg->nets[order[k]];
        printf("STA: connecting to \"%s\" (priority %d)...\n", n->ssid, n->priority);
        if (sta_connect_one(n, 20000)) {
            struct netif *nif = &cyw43_state.netif[CYW43_ITF_STA];
            printf("STA: connected to \"%s\". IP=%s\n", n->ssid, ip4addr_ntoa(netif_ip4_addr(nif)));
#ifdef ENABLE_STATUSD
            char poll_desc[96];
            snprintf(poll_desc, sizeof(poll_desc), "http://%s:%d/%s.txt",
                     POLL_HOST, (int)POLL_PORT, cfg->slug);
            statusd_start(cfg->slug, poll_desc);  /* status page + mDNS (opt-in) */
#endif
            run_poll_loop(cfg->slug);              /* M4: never returns */
        }
        printf("STA: connect to \"%s\" failed\n", n->ssid);
    }
    reboot_to_portal();
}

/* ── AP / captive portal (M1) ─────────────────────────────────────────────── */

/* Raise the setup AP + DHCP/DNS/HTTP portal. Reboots once a config is saved. */
static void run_portal(void) {
    /* SSID is configured at build time: -DHEALTHBAR_NAME=shen -> "healthbar-setup-shen". */
    char ap_name[40];
    if (HEALTHBAR_NAME[0])
        snprintf(ap_name, sizeof(ap_name), "healthbar-setup-%s", HEALTHBAR_NAME);
    else
        snprintf(ap_name, sizeof(ap_name), "healthbar-setup");
    for (char *c = ap_name; *c; c++) *c = (char)tolower((unsigned char)*c);

    printf("Raising AP: %s\n", ap_name);
    cyw43_arch_enable_ap_mode(ap_name, NULL, CYW43_AUTH_OPEN);

    ip_addr_t gw, mask;
    IP4_ADDR(&gw,   192, 168, 4, 1);
    IP4_ADDR(&mask, 255, 255, 255, 0);

    portal_state_t portal = {0};
    IP4_ADDR(&portal.gw, 192, 168, 4, 1);
    portal.reboot_pending = false;

    dhcp_server_t dhcp;
    dhcp_server_init(&dhcp, &gw, &mask);
    dns_server_t dns;
    dns_server_init(&dns, &gw);

    if (!portal_open(&portal)) {
        printf("ERROR: portal_open failed\n");
        cyw43_arch_deinit();
        while (true) tight_loop_contents();
    }

    printf("Portal running. Connect to '%s' and visit http://%s/\n", ap_name, AP_IP_ADDR);

    while (!portal.reboot_pending) {
        watchdog_update();
        provision_poll();   /* allow `just set ...` over USB while unprovisioned */
        cyw43_arch_poll();
        sleep_ms(10);
    }

    printf("Config saved — rebooting into STA mode...\n");
    sleep_ms(500);  /* give TCP time to flush the SAVED_PAGE response */
    watchdog_reboot(0, 0, 100);
    while (true) tight_loop_contents();
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(void) {
    stdio_init_all();
    sleep_ms(2000);  /* let USB CDC enumerate */

    printf("healthbar (M3: STA connect) — starting\n");

    /* Portal-force flag, set by a previous failed STA connect (survives the
     * watchdog reboot in a scratch register; 0 on a real power-on). */
    bool force_portal = (watchdog_hw->scratch[0] == PORTAL_FORCE_MAGIC);
    if (force_portal) watchdog_hw->scratch[0] = 0;

    /* Load persisted config. Don't print the PSK — only ssid + priority. */
    persist_t cfg;
    bool have = config_load(&cfg);

#ifdef DEV_SEED_CONFIG
    /* DEV-ONLY: when built with -DDEV_SEED_CONFIG, seed a config on first boot
     * from build-time creds so the device can be provisioned without a phone
     * (for M3/M4 bring-up). Never enabled in a normal build. Exercises the full
     * config_save -> config_load round trip. */
    if (!have) {
        printf("DEV_SEED: no config; writing seed (ssid=%s slug=%s)\n",
               DEV_SEED_SSID, DEV_SEED_SLUG);
        persist_t seed = {0};
        seed.net_count = 1;
        strncpy(seed.nets[0].ssid, DEV_SEED_SSID, sizeof(seed.nets[0].ssid) - 1);
        strncpy(seed.nets[0].psk,  DEV_SEED_PSK,  sizeof(seed.nets[0].psk) - 1);
        seed.nets[0].priority = 1;
        strncpy(seed.slug, DEV_SEED_SLUG, sizeof(seed.slug) - 1);
        printf("DEV_SEED: config_save %s\n", config_save(&seed) ? "OK" : "FAILED");
        have = config_load(&cfg);
        printf("DEV_SEED: readback %s\n", have ? "OK" : "FAILED");
    }
#endif

    if (have) {
        printf("config_load: VALID — slug='%s', %d network(s):\n", cfg.slug, cfg.net_count);
        for (int i = 0; i < cfg.net_count; i++)
            printf("  net[%d]: ssid=\"%s\" priority=%d\n", i, cfg.nets[i].ssid, cfg.nets[i].priority);
    } else {
        printf("config_load: none/invalid — raising setup portal\n");
    }

    /* ONE cyw43_arch_init per boot (cold-boot-hang constraint). The radio plays
     * exactly one role this boot: STA (connect) OR AP (portal), never both, and
     * we never scan-while-AP on a cold cyw43 — that was the MicroPython hang. */
    if (cyw43_arch_init()) {
        printf("ERROR: cyw43_arch_init failed\n");
        while (true) tight_loop_contents();
    }

    /* M5: start the LED render engine on core1 (boot sweep until states change).
     * core1 only touches the LED PIO + shared health state. */
    health_init();
    leds_launch();

    /* Hardware watchdog: a genuine core0 hang/panic reboots after ~8s instead of
     * bricking until a manual BOOTSEL. Fed by core0's loops only — NOT core1:
     * feeding from the always-running render core would mask a core0 hang (the
     * bug the MicroPython build had). Long ops (wifi connect, HTTP) feed as they wait. */
    watchdog_enable(8000, false);  /* pause_on_debug=false: always count, even if SWD is sensed */

    if (have && cfg.net_count > 0 && !force_portal) {
        printf("Booting STA mode.\n");
        run_sta_mode(&cfg);     /* connects + holds, or reboots to portal; never returns */
    } else {
        if (force_portal)
            printf("Forced into setup portal (previous STA attempts failed).\n");
        else if (have && cfg.net_count == 0)
            printf("Config has a slug but no WiFi — raising portal (use `just set wifi`).\n");
        run_portal();           /* AP + captive portal; reboots once config is saved */
    }
    return 0;
}
