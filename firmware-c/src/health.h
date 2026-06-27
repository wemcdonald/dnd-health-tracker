/**
 * health.h — shared health state between core0 (network) and core1 (LEDs).
 *
 * core0 writes (poll result + connection state); core1 reads a snapshot under a
 * spinlock and renders. The server sends continuous HP (`cur`/`max`/`temp`); the
 * device derives the bar fill itself (see leds.c). `temp` is a separate buffer on
 * top of `cur`, carried for reactive animations (Batch 2) — not the steady fill.
 */
#ifndef HEALTHBAR_HEALTH_H
#define HEALTHBAR_HEALTH_H

#include <stdint.h>

#define NUM_LEDS 16

typedef enum {
    ST_BOOT = 0,    // startup sweep
    ST_CONNECTING,  // scanning dot (joining wifi)
    ST_LIVE,        // showing HP from the server
    ST_OFFLINE,     // poll/connection failed: breathing
} anim_state_t;

typedef struct {
    anim_state_t state;
    int          cur;         // current HP, 0..max
    int          max;         // max HP, >= 1 when live
    int          temp;        // temporary HP buffer on top of cur
    int          age_s;       // upstream staleness from line 1
    uint32_t     updated_ms;  // when core0 last set HP (for flash detection)
} health_t;

/* core0 API (thread-safe via spinlock). */
void     health_init(void);
void     health_set_state(anim_state_t st);
void     health_set_hp(int cur, int max, int temp, int age_s);  // sets ST_LIVE + updated_ms
health_t health_snapshot(void);

#endif /* HEALTHBAR_HEALTH_H */
