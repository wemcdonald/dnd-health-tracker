/**
 * configform.c — shared config web form + save. See configform.h.
 */
#include "configform.h"
#include "config.h"
#include "health.h"
#include "leds.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/netif.h"

/* ── helpers ──────────────────────────────────────────────────────────────── */

static int hexd(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

static void url_decode(char *dst, const char *src, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; src[i] && o + 1 < cap; i++) {
        if (src[i] == '+') dst[o++] = ' ';
        else if (src[i] == '%' && src[i + 1] && src[i + 2]) {
            int hi = hexd(src[i + 1]), lo = hexd(src[i + 2]);
            if (hi >= 0 && lo >= 0) { dst[o++] = (char)((hi << 4) | lo); i += 2; }
            else dst[o++] = src[i];
        } else dst[o++] = src[i];
    }
    dst[o] = '\0';
}

static void html_attr(char *dst, const char *src, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; src[i] && o + 7 < cap; i++) {
        char c = src[i];
        if (c == '&')      { memcpy(dst + o, "&amp;", 5);  o += 5; }
        else if (c == '<') { memcpy(dst + o, "&lt;", 4);   o += 4; }
        else if (c == '>') { memcpy(dst + o, "&gt;", 4);   o += 4; }
        else if (c == '"') { memcpy(dst + o, "&quot;", 6); o += 6; }
        else dst[o++] = c;
    }
    dst[o] = '\0';
}

static void get_param(const char *qs, const char *key, char *out, size_t cap) {
    out[0] = '\0';
    size_t klen = strlen(key);
    const char *p = qs;
    while (p && *p) {
        while (*p == '&') p++;
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *val = p + klen + 1;
            const char *end = strchr(val, '&');
            size_t vlen = end ? (size_t)(end - val) : strlen(val);
            char raw[200];
            if (vlen >= sizeof(raw)) vlen = sizeof(raw) - 1;
            memcpy(raw, val, vlen);
            raw[vlen] = '\0';
            url_decode(out, raw, cap);
            return;
        }
        p = strchr(p, '&');
    }
}

static const char *state_str(anim_state_t st) {
    switch (st) {
        case ST_BOOT:       return "boot";
        case ST_CONNECTING: return "connecting";
        case ST_LIVE:       return "live";
        case ST_OFFLINE:    return "offline";
        default:            return "?";
    }
}

/* ── page ─────────────────────────────────────────────────────────────────── */

static const char PAGE_HEAD[] =
    "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\n\r\n"
    "<!DOCTYPE html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>%s</title><style>"
    "body{font-family:sans-serif;margin:20px auto;max-width:420px;padding:0 14px;color:#222}"
    "h2{margin:.2em 0}h3{margin:1.2em 0 .3em}"
    "table{border-collapse:collapse;width:100%%;margin:.5em 0}"
    "td{padding:.3rem .5rem;border-bottom:1px solid #eee}td:first-child{color:#777;width:8em}"
    "label{display:block;font-weight:bold;font-size:13px;margin-top:8px}"
    "input{width:100%%;padding:9px;margin-top:3px;box-sizing:border-box;font-size:16px}"
    ".net{margin:0 0 16px;padding:12px;border:1px solid #ccc;border-radius:8px}"
    ".net b{color:#555}button{width:100%%;padding:13px;font-size:16px;margin-top:10px}"
    "small{color:#666}</style></head><body>";

int configform_page(char *out, int cap, bool with_status, const char *slug, const char *poll_desc) {
    persist_t cfg;
    bool have = config_load(&cfg);

    int n = snprintf(out, cap, PAGE_HEAD, with_status ? "healthbar" : "Healthbar Setup");

    if (with_status) {
        health_t h = health_snapshot();
        uint32_t c1_frames; int c1_state, c1_lit;
        leds_diag(&c1_frames, &c1_state, &c1_lit);
        uint32_t up = to_ms_since_boot(get_absolute_time()) / 1000;
        const char *ip = ip4addr_ntoa(netif_ip4_addr(&cyw43_state.netif[CYW43_ITF_STA]));
        char slug_esc[64];
        html_attr(slug_esc, slug ? slug : "", sizeof(slug_esc));
        n += snprintf(out + n, cap - n,
            "<h2>healthbar: %s</h2><table>"
            "<tr><td>state</td><td><b>%s</b></td></tr>"
            "<tr><td>LEDs lit</td><td>%d / %d</td></tr>"
            "<tr><td>upstream age</td><td>%d s</td></tr>"
            "<tr><td>IP</td><td>%s</td></tr>"
            "<tr><td>polling</td><td><small>%s</small></td></tr>"
            "<tr><td>uptime</td><td>%lu s</td></tr>"
            "<tr><td>core1</td><td>frames=%lu, last %s %d/%d</td></tr></table>",
            slug_esc, state_str(h.state), h.lit, NUM_LEDS, h.age_s, ip,
            poll_desc ? poll_desc : "", (unsigned long)up,
            (unsigned long)c1_frames, state_str((anim_state_t)c1_state), c1_lit, NUM_LEDS);
    } else {
        n += snprintf(out + n, cap - n, "<h2>Healthbar Setup</h2>");
    }

    /* current slug pre-fill */
    char cur_slug[64] = "";
    if (have) html_attr(cur_slug, cfg.slug, sizeof(cur_slug));

    n += snprintf(out + n, cap - n,
        "<form method=GET action=/save>"
        "<h3>Character</h3>"
        "<label>name (slug)</label>"
        "<input name=slug value=\"%s\" autocapitalize=off autocorrect=off placeholder='e.g. thorin'>"
        "<h3>WiFi networks</h3>"
        "<small>Leave a password blank to keep the saved one. Priority: lower = tried first.</small>",
        cur_slug);

    for (int i = 0; i < MAX_NETS && n < cap - 400; i++) {
        char ssid_esc[80] = "";
        int prio = i + 1;
        if (have && i < cfg.net_count) {
            html_attr(ssid_esc, cfg.nets[i].ssid, sizeof(ssid_esc));
            prio = cfg.nets[i].priority;
        }
        n += snprintf(out + n, cap - n,
            "<div class=net><b>Network %d%s</b>"
            "<label>SSID</label><input name=s%d value=\"%s\" autocapitalize=off autocorrect=off>"
            "<label>Password</label><input name=p%d type=password placeholder=\"(unchanged)\">"
            "<label>Priority</label><input name=r%d type=number value=%d></div>",
            i + 1, i == 0 ? "" : " (optional)", i + 1, ssid_esc, i + 1, i + 1, prio);
    }

    n += snprintf(out + n, cap - n,
        "<button type=submit>Save &amp; reboot</button></form>");

    if (with_status) {
        n += snprintf(out + n, cap - n,
            "<form method=POST action=/reconfigure "
            "onsubmit=\"return confirm('Reboot into AP setup mode?')\">"
            "<button>Reboot to AP setup</button></form>");
    }
    n += snprintf(out + n, cap - n, "</body></html>");
    return n;
}

int configform_simple(char *out, int cap, const char *title, const char *body) {
    return snprintf(out, cap,
        "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\n\r\n"
        "<!DOCTYPE html><html><head><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'></head>"
        "<body style='font-family:sans-serif;margin:24px'><h2>%s</h2><p>%s</p></body></html>",
        title, body);
}

bool configform_save(const char *qs) {
    persist_t cur;
    bool have = config_load(&cur);
    persist_t neu;
    memset(&neu, 0, sizeof(neu));

    char slug[48];
    get_param(qs, "slug", slug, sizeof(slug));
    if (slug[0]) strncpy(neu.slug, slug, sizeof(neu.slug) - 1);
    else if (have) strncpy(neu.slug, cur.slug, sizeof(neu.slug) - 1);

    int n = 0;
    for (int i = 0; i < MAX_NETS; i++) {
        char ks[4], kp[4], kr[4], s[33], p[65], r[8];
        snprintf(ks, sizeof(ks), "s%d", i + 1);
        snprintf(kp, sizeof(kp), "p%d", i + 1);
        snprintf(kr, sizeof(kr), "r%d", i + 1);
        get_param(qs, ks, s, sizeof(s));
        if (!s[0]) continue;
        get_param(qs, kp, p, sizeof(p));
        get_param(qs, kr, r, sizeof(r));
        strncpy(neu.nets[n].ssid, s, sizeof(neu.nets[n].ssid) - 1);
        if (p[0]) {
            strncpy(neu.nets[n].psk, p, sizeof(neu.nets[n].psk) - 1);
        } else if (have) {
            for (int j = 0; j < cur.net_count; j++)
                if (strcmp(cur.nets[j].ssid, s) == 0) {
                    strncpy(neu.nets[n].psk, cur.nets[j].psk, sizeof(neu.nets[n].psk) - 1);
                    break;
                }
        }
        neu.nets[n].priority = (uint8_t)(r[0] ? atoi(r) : (i + 1));
        n++;
    }
    if (n == 0) return false;
    neu.net_count = (uint16_t)n;
    return config_save(&neu);
}
