/**
 * config.h — persisted device configuration (M2).
 *
 * Stores the user's WiFi networks + character slug in the last sector of flash,
 * validated by a magic number + version + CRC32. Written only in setup mode
 * (before core1 is launched), so no multicore-lockout dance is needed.
 *
 * Single-sector (not A/B double-buffered): if a write is interrupted, the CRC
 * fails on next boot and we treat it as "no config" -> re-enter the portal. For
 * a desk device that's an acceptable failure mode; A/B is a future hardening.
 *
 * NUM_LEDS / GPIO / poll cadence are compile-time constants, NOT persisted —
 * only the user-configurable wifi nets + slug live here.
 */
#ifndef HEALTHBAR_CONFIG_H
#define HEALTHBAR_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#define MAX_NETS 5

typedef struct {
    char    ssid[33];   // 32 chars + NUL
    char    psk[64];    // 63 chars + NUL
    uint8_t priority;   // lower = tried first
} wifi_net_t;

typedef struct {
    uint32_t   magic;
    uint16_t   version;
    uint16_t   net_count;          // 1..MAX_NETS
    wifi_net_t nets[MAX_NETS];
    char       slug[48];
    uint32_t   crc32;              // over all preceding bytes
} persist_t;

/* Load + validate config from flash. Returns true and fills *out if valid. */
bool config_load(persist_t *out);

/* Stamp magic/version/crc on *cfg and write it to flash. Returns true if the
 * written image reads back valid. Disables interrupts during the flash op (and
 * locks out core1 if it has been started — see config_set_core1_running). */
bool config_save(persist_t *cfg);

/* Tell config_save whether core1 is running as a flash lockout victim. Must be
 * true before any flash write happens while core1 executes from XIP flash. */
void config_set_core1_running(bool running);

#endif /* HEALTHBAR_CONFIG_H */
