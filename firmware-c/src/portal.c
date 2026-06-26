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
 * M1 behaviour on form submit:
 *   Parses and PRINTS the captured values over serial, returns a "Saved
 *   (M1: not yet persisted), rebooting..." page, then calls watchdog_reboot.
 *   Flash persistence is M2.
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
#define POLL_HOST "public.willflix.com"
#endif
#ifndef POLL_PORT
#define POLL_PORT 80
#endif

/* How long to keep an idle HTTP connection open before closing it. */
#define HTTP_POLL_INTERVAL_S 5

/* ── URL decoder ──────────────────────────────────────────────────────────── */

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

/* Decode %xx and '+' in-place. Returns pointer to dst for convenience. */
static char *url_decode(char *dst, const char *src, size_t dst_size) {
    size_t out = 0;
    for (size_t i = 0; src[i] && out + 1 < dst_size; i++) {
        if (src[i] == '+') {
            dst[out++] = ' ';
        } else if (src[i] == '%' && src[i+1] && src[i+2]) {
            int hi = hex_digit(src[i+1]);
            int lo = hex_digit(src[i+2]);
            if (hi >= 0 && lo >= 0) {
                dst[out++] = (char)((hi << 4) | lo);
                i += 2;
            } else {
                dst[out++] = src[i];
            }
        } else {
            dst[out++] = src[i];
        }
    }
    dst[out] = '\0';
    return dst;
}

/* Extract the value of a named key from a '&'-separated query string into the
 * caller-provided buffer (URL-decoded). Writes "" if the key is absent.
 *
 * NOTE: writes into the caller's buffer rather than a shared static one — an
 * earlier version returned a pointer into a single static buffer, so calling it
 * for s/p/r in a row made all three alias the last value (the priority). */
static void get_param(const char *qs, const char *key, char *out, size_t out_size) {
    out[0] = '\0';
    size_t klen = strlen(key);
    const char *p = qs;
    while (p && *p) {
        /* Skip leading & */
        while (*p == '&') p++;
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *val = p + klen + 1;
            const char *end = strchr(val, '&');
            size_t vlen = end ? (size_t)(end - val) : strlen(val);
            char raw[128];
            if (vlen >= sizeof(raw)) vlen = sizeof(raw) - 1;
            memcpy(raw, val, vlen);
            raw[vlen] = '\0';
            url_decode(out, raw, out_size);
            return;
        }
        /* Advance past this key=value pair. */
        p = strchr(p, '&');
    }
}

/* ── HTML pages ───────────────────────────────────────────────────────────── */

/* Setup form: 3 network groups + slug. Stacked fields, mobile-friendly.
 * Well under TCP_SND_BUF (8*1460), sent in one tcp_write. */
static const char SETUP_PAGE[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/html; charset=utf-8\r\n"
    "Connection: close\r\n"
    "\r\n"
    "<!DOCTYPE html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Healthbar Setup</title><style>"
    "body{font-family:sans-serif;margin:16px auto;max-width:420px;padding:0 14px;color:#222}"
    "h2{margin:.2em 0}"
    "label{display:block;font-weight:bold;font-size:13px;margin-top:10px}"
    "input{width:100%;padding:10px;margin-top:4px;box-sizing:border-box;font-size:16px}"
    ".net{margin:0 0 28px;padding:14px;border:1px solid #ccc;border-radius:8px}"
    ".net b{font-size:14px;color:#555}"
    "button{width:100%;padding:14px;font-size:16px;margin-top:8px}"
    "</style></head><body>"
    "<h2>Healthbar Setup</h2>"
    "<form method=GET action=/save>"
    "<div class=net><b>WiFi network 1</b>"
    "<label>SSID</label><input name=s1 autocapitalize=off autocorrect=off>"
    "<label>Password</label><input name=p1>"
    "<label>Priority</label><input name=r1 type=number value=1></div>"
    "<div class=net><b>WiFi network 2 (optional)</b>"
    "<label>SSID</label><input name=s2 autocapitalize=off autocorrect=off>"
    "<label>Password</label><input name=p2>"
    "<label>Priority</label><input name=r2 type=number value=2></div>"
    "<div class=net><b>WiFi network 3 (optional)</b>"
    "<label>SSID</label><input name=s3 autocapitalize=off autocorrect=off>"
    "<label>Password</label><input name=p3>"
    "<label>Priority</label><input name=r3 type=number value=3></div>"
    "<label>Character slug</label>"
    "<input name=slug value='" HEALTHBAR_NAME "' autocapitalize=off autocorrect=off "
    "placeholder='e.g. thorin-oakenshield'>"
    "<button type=submit>Save &amp; Reboot</button>"
    "</form></body></html>";

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

static const char SAVED_PAGE[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/html; charset=utf-8\r\n"
    "Connection: close\r\n"
    "\r\n"
    "<!DOCTYPE html><html><body>"
    "<h2>Saved (M1: not yet persisted), rebooting...</h2>"
    "<p>You can close this window.</p>"
    "</body></html>";

static const char INVALID_PAGE[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/html; charset=utf-8\r\n"
    "Connection: close\r\n"
    "\r\n"
    "<!DOCTYPE html><html><body>"
    "<h2>Invalid input</h2>"
    "<p>At least one SSID and the character slug are required.</p>"
    "<a href='/'>Back</a>"
    "</body></html>";

/* ── TCP server state ─────────────────────────────────────────────────────── */

typedef struct {
    struct tcp_pcb *server_pcb;
    bool reboot_pending;
    ip_addr_t gw;
} portal_state_t;

typedef struct {
    struct tcp_pcb *pcb;
    int sent_len;
    char request_buf[512];  /* enough for GET line + headers */
    /* Response to send (points into static strings above). */
    const char *response;
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

/* ── Form submit handler ──────────────────────────────────────────────────── */

static bool handle_save(const char *qs, portal_state_t *portal) {
    persist_t cfg = {0};

    char key_s[4], key_p[4], key_r[4];
    for (int i = 0; i < MAX_NETS; i++) {
        snprintf(key_s, sizeof(key_s), "s%d", i + 1);
        snprintf(key_p, sizeof(key_p), "p%d", i + 1);
        snprintf(key_r, sizeof(key_r), "r%d", i + 1);

        char s[33], p[65], r[8];
        get_param(qs, key_s, s, sizeof(s));
        get_param(qs, key_p, p, sizeof(p));
        get_param(qs, key_r, r, sizeof(r));

        if (s[0] != '\0') {
            wifi_net_t *n = &cfg.nets[cfg.net_count];
            strncpy(n->ssid, s, sizeof(n->ssid) - 1);
            strncpy(n->psk,  p, sizeof(n->psk) - 1);
            n->priority = (uint8_t)(r[0] ? atoi(r) : (i + 1));
            cfg.net_count++;
        }
    }

    get_param(qs, "slug", cfg.slug, sizeof(cfg.slug));

    /* Validate. */
    if (cfg.net_count == 0 || cfg.slug[0] == '\0') {
        return false;
    }

    printf("\n=== Portal form submit ===\n");
    printf("slug: %s\n", cfg.slug);
    for (int i = 0; i < cfg.net_count; i++) {
        printf("net[%d]: ssid=\"%s\" psk=\"%s\" priority=%d\n",
               i, cfg.nets[i].ssid, cfg.nets[i].psk, cfg.nets[i].priority);
    }

    /* M2: persist to flash and confirm it reads back valid. */
    bool ok = config_save(&cfg);
    printf("config_save: %s\n", ok ? "OK (persisted to flash)" : "FAILED (crc/readback)");
    printf("=========================\n");
    if (!ok) return false;

    portal->reboot_pending = true;
    return true;
}

/* ── HTTP receive callback ────────────────────────────────────────────────── */

static err_t tcp_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    conn_state_t *cs = (conn_state_t *)arg;

    if (!p) {
        return conn_close(cs, pcb, ERR_OK);
    }

    if (p->tot_len > 0) {
        /* Copy into our buffer (truncate if oversized). */
        size_t copy_len = p->tot_len;
        if (copy_len > sizeof(cs->request_buf) - 1)
            copy_len = sizeof(cs->request_buf) - 1;
        pbuf_copy_partial(p, cs->request_buf, (u16_t)copy_len, 0);
        cs->request_buf[copy_len] = '\0';
    }
    pbuf_free(p);
    tcp_recved(pcb, p->tot_len); /* ACK the bytes */

    /* Parse the first line: GET <path>[?<qs>] HTTP/... */
    if (strncmp(cs->request_buf, "GET ", 4) != 0) {
        /* Not a GET — just close. */
        return conn_close(cs, pcb, ERR_OK);
    }

    char *path_start = cs->request_buf + 4;
    char *space = strchr(path_start, ' ');
    if (space) *space = '\0';

    char *qs = strchr(path_start, '?');
    if (qs) *qs++ = '\0';  /* qs now points to query string, path is clean */

    /* Route: /save = form submit; / = form; everything else = catch-all redirect */
    if (strcmp(path_start, "/save") == 0 && qs) {
        bool ok = handle_save(qs, cs->portal);
        cs->response = ok ? SAVED_PAGE : INVALID_PAGE;
    } else if (strcmp(path_start, "/") == 0 || strcmp(path_start, "/index.html") == 0) {
        cs->response = SETUP_PAGE;
    } else {
        cs->response = CATCHALL_PAGE;
    }

    cs->response_len = (int)strlen(cs->response);
    cs->sent_len = 0;

    cyw43_arch_lwip_begin();
    err_t write_err = tcp_write(pcb, cs->response, (u16_t)cs->response_len, TCP_WRITE_FLAG_COPY);
    if (write_err == ERR_OK) {
        write_err = tcp_output(pcb);
    }
    cyw43_arch_lwip_end();

    if (write_err != ERR_OK) {
        return conn_close(cs, pcb, write_err);
    }

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

/* M4: poll the server for "<lit> <age>" every ~2s and report over serial.
 * The LED render (M5) will consume `lit`; for now we log it. Never returns. */
static void run_poll_loop(const char *slug) {
    char path[80];
    snprintf(path, sizeof(path), "/dnd/%s.txt", slug);

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
        }
        sleep_ms(2000);
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
            snprintf(poll_desc, sizeof(poll_desc), "http://%s:%d/dnd/%s.txt",
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
    watchdog_enable(8000, true);

    if (have && !force_portal) {
        printf("Booting STA mode.\n");
        run_sta_mode(&cfg);     /* connects + holds, or reboots to portal; never returns */
    } else {
        if (force_portal)
            printf("Forced into setup portal (previous STA attempts failed).\n");
        run_portal();           /* AP + captive portal; reboots once config is saved */
    }
    return 0;
}
