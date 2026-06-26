/**
 * provision.c — USB-serial provisioning. See provision.h.
 */
#include "provision.h"
#include "config.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "hardware/watchdog.h"

#define LINE_MAX 256

/* ── URL decode (percent + '+'), self-contained ───────────────────────────── */

static int hexd(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

static void url_decode(char *dst, const char *src, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; src[i] && o + 1 < cap; i++) {
        if (src[i] == '+') {
            dst[o++] = ' ';
        } else if (src[i] == '%' && src[i + 1] && src[i + 2]) {
            int hi = hexd(src[i + 1]), lo = hexd(src[i + 2]);
            if (hi >= 0 && lo >= 0) { dst[o++] = (char)((hi << 4) | lo); i += 2; }
            else dst[o++] = src[i];
        } else {
            dst[o++] = src[i];
        }
    }
    dst[o] = '\0';
}

/* ── command handling ─────────────────────────────────────────────────────── */

static void apply_and_reboot(const char *what) {
    printf("OK %s — rebooting to apply\n", what);
    sleep_ms(200);  /* let the reply flush over USB before the reset */
    watchdog_reboot(0, 0, 100);
    while (true) tight_loop_contents();
}

static void process(char *line) {
    /* Split on spaces (in place). Values are percent-encoded, so embedded
     * spaces arrive as %20 and don't split. */
    char *argv[2 + 2 * MAX_NETS];
    int argc = 0;
    char *p = line;
    while (*p && argc < (int)(sizeof(argv) / sizeof(argv[0]))) {
        while (*p == ' ') *p++ = '\0';
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ') p++;
    }
    if (argc == 0) return;

    if (strcmp(argv[0], "name") == 0 && argc >= 2) {
        persist_t cfg;
        if (!config_load(&cfg)) memset(&cfg, 0, sizeof(cfg));
        url_decode(cfg.slug, argv[1], sizeof(cfg.slug));
        if (config_save(&cfg)) apply_and_reboot("name");
        else printf("ERR save failed\n");

    } else if (strcmp(argv[0], "wifi") == 0 && argc >= 3) {
        persist_t cfg;
        if (!config_load(&cfg)) memset(&cfg, 0, sizeof(cfg));
        int n = 0;
        for (int i = 1; i + 1 < argc && n < MAX_NETS; i += 2) {
            url_decode(cfg.nets[n].ssid, argv[i],     sizeof(cfg.nets[n].ssid));
            url_decode(cfg.nets[n].psk,  argv[i + 1], sizeof(cfg.nets[n].psk));
            cfg.nets[n].priority = (uint8_t)(n + 1);
            n++;
        }
        if (n == 0) { printf("ERR no networks\n"); return; }
        cfg.net_count = (uint16_t)n;
        if (config_save(&cfg)) { printf("OK wifi: %d network(s)\n", n); apply_and_reboot("wifi"); }
        else printf("ERR save failed\n");

    } else if (strcmp(argv[0], "show") == 0) {
        persist_t cfg;
        if (config_load(&cfg)) {
            printf("slug=%s nets=%d\n", cfg.slug, cfg.net_count);
            for (int i = 0; i < cfg.net_count; i++)
                printf("  [%d] ssid=%s priority=%d\n", i, cfg.nets[i].ssid, cfg.nets[i].priority);
        } else {
            printf("no config\n");
        }

    } else if (strcmp(argv[0], "reboot") == 0) {
        apply_and_reboot("reboot");

    } else {
        printf("ERR unknown command '%s' (name|wifi|show|reboot)\n", argv[0]);
    }
}

void provision_poll(void) {
    static char buf[LINE_MAX];
    static int len = 0;
    int c;
    while ((c = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) {
        if (c == '\r') continue;
        if (c == '\n') {
            buf[len] = '\0';
            if (len > 0) process(buf);
            len = 0;
        } else if (len < LINE_MAX - 1) {
            buf[len++] = (char)c;
        } else {
            len = 0;  /* overflow — drop the line */
        }
    }
}
