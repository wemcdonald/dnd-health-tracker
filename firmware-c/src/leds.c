/**
 * leds.c — WS2812 render engine, runs on core1.
 *
 * Pure time-based animations of the shared health snapshot, so frame rate never
 * affects motion. The server sends continuous HP (cur/max/temp); the device
 * derives the bar fill from cur/max. Color comes from the fill fraction.
 *
 * 14-LED bar: the physical strip is NUM_LEDS (16) but the enclosure's end-caps
 * hide physical LED 0 and 15, so all bar math is logical 0..BAR_LEN-1 written to
 * physical BAR_FIRST + i, and show() always forces the two end-caps off.
 *
 * Steady render: a bipolar bar — GREEN (health) from the left, RED (hurt) from
 * the right — with "fairness" (>= 1 LED of each while both exist). HP is read
 * purely from the green/red split; both colors render at full intensity. A
 * hit/heal "splash then drain" animation fires on any change in current HP.
 * Below 50% health the red pulses (faster + deeper the more hurt). Still deferred
 * from the full vision (see memory `led-color-model-vision`): the healthy green
 * shimmer. core1 never touches cyw43, lwIP, or flash.
 */
#include "leds.h"
#include "health.h"
#include "config.h"

#include <math.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/pio.h"
#include "hardware/sync.h"

#include "ws2812.pio.h"

#define LED_GPIO        2
#define FRAME_MS        33          // ~30 fps
#define BRIGHTNESS      0.5f        // global scale
#define BAR_FIRST       1           // physical index of logical LED 0 (end-cap at 0)
#define BAR_LEN         14          // visible LEDs (physical 1..14; 0 and 15 hidden)
#define FLASH_MS        800         // total hit/heal animation length
#define FLASH_ATTACK_MS 120         // surge-to-full portion; the rest drains down
#define PULSE_F_LOW     0.0625f     // 2/32: fastest + deepest red pulse at/below this
#define PULSE_PERIOD_HI 15.0f       // seconds/cycle at 50% health (pulse onset)
#define PULSE_PERIOD_LO 6.0f        // seconds/cycle at PULSE_F_LOW or below

/* theme colors (r,g,b) */
typedef struct { float r, g, b; } rgb_t;
static const rgb_t HP_HIGH = { 0.0f,   0.86f, 0.235f }; // #00dc3c
static const rgb_t HP_MID  = { 0.90f,  0.706f, 0.0f };  // #e6b400
static const rgb_t RED     = { 1.0f, 0.0f, 0.0f };
static const rgb_t GREEN   = { 0.0f, 1.0f, 0.0f };

/* ── shared state (spinlock-guarded) ──────────────────────────────────────── */

static health_t   g_health;
static spin_lock_t *g_lock;

/* DIAG (M5 bring-up): core1 publishes its own liveness here. Plain volatiles,
 * single 32-bit aligned writes from core1 / reads from core0 — atomic on M33,
 * no lock needed for a liveness probe. */
static volatile uint32_t g_frames     = 0;
static volatile int      g_last_state = -1;
static volatile int      g_last_cur   = -1;

void leds_diag(uint32_t *frames, int *last_state, int *last_cur) {
    if (frames)     *frames     = g_frames;
    if (last_state) *last_state = g_last_state;
    if (last_cur)   *last_cur   = g_last_cur;
}

void health_init(void) {
    g_lock = spin_lock_init(spin_lock_claim_unused(true));
    g_health.state = ST_BOOT;
    g_health.cur = 0;
    g_health.max = 0;
    g_health.temp = 0;
    g_health.age_s = 0;
    g_health.updated_ms = 0;
}

void health_set_state(anim_state_t st) {
    uint32_t save = spin_lock_blocking(g_lock);
    g_health.state = st;
    spin_unlock(g_lock, save);
}

void health_set_hp(int cur, int max, int temp, int age_s) {
    uint32_t save = spin_lock_blocking(g_lock);
    g_health.state = ST_LIVE;
    g_health.cur = cur;
    g_health.max = max;
    g_health.temp = temp;
    g_health.age_s = age_s;
    g_health.updated_ms = to_ms_since_boot(get_absolute_time());
    spin_unlock(g_lock, save);
}

health_t health_snapshot(void) {
    uint32_t save = spin_lock_blocking(g_lock);
    health_t h = g_health;
    spin_unlock(g_lock, save);
    return h;
}

/* ── color helpers ────────────────────────────────────────────────────────── */

static inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static rgb_t lerp(rgb_t a, rgb_t b, float t) {
    t = clampf(t, 0.0f, 1.0f);
    return (rgb_t){ a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t };
}

static inline uint32_t pack_grb(rgb_t c, float scale) {
    uint8_t r = (uint8_t)(clampf(c.r * scale, 0.0f, 1.0f) * 255.0f);
    uint8_t g = (uint8_t)(clampf(c.g * scale, 0.0f, 1.0f) * 255.0f);
    uint8_t b = (uint8_t)(clampf(c.b * scale, 0.0f, 1.0f) * 255.0f);
    return ((uint32_t)g << 16) | ((uint32_t)r << 8) | (uint32_t)b; // GRB
}

/* ── PIO output ───────────────────────────────────────────────────────────── */

static PIO  s_pio;
static uint s_sm;

static inline void put_pixel(uint32_t grb) {
    pio_sm_put_blocking(s_pio, s_sm, grb << 8u);
}

/* Push the frame to the strip, forcing the two hidden end-caps (physical 0 and
 * NUM_LEDS-1) off regardless of what the animation wrote. */
static void show(const rgb_t frame[NUM_LEDS], float scale) {
    const rgb_t off = {0};
    for (int i = 0; i < NUM_LEDS; i++) {
        bool endcap = (i == 0 || i == NUM_LEDS - 1);
        put_pixel(pack_grb(endcap ? off : frame[i], scale));
    }
}

/* ── render ───────────────────────────────────────────────────────────────── */

/* All bar animations write logical 0..BAR_LEN-1 into the physical strip via
 * BAR_FIRST + i; the end-caps (physical 0 and NUM_LEDS-1) are kept dark by
 * show(). */
static void render(const health_t *h, uint32_t now_ms, int *prev_cur,
                   uint32_t *flash_start_ms, bool *flash_is_damage) {
    rgb_t frame[NUM_LEDS] = {0};
    float t = now_ms / 1000.0f;

    switch (h->state) {
    case ST_BOOT: {
        int pos = (now_ms / 60) % BAR_LEN;        // sweeping dot
        frame[BAR_FIRST + pos] = HP_HIGH;
        show(frame, BRIGHTNESS);
        return;
    }
    case ST_CONNECTING: {
        int span = BAR_LEN - 1;
        int p = (now_ms / 80) % (2 * span);
        int pos = p < span ? p : (2 * span - p);  // bounce
        frame[BAR_FIRST + pos] = HP_MID;
        show(frame, BRIGHTNESS);
        return;
    }
    case ST_OFFLINE: {
        float br = 0.15f + 0.15f * (0.5f + 0.5f * sinf(2.0f * (float)M_PI * 0.25f * t));
        for (int i = 0; i < BAR_LEN; i++) frame[BAR_FIRST + i] = HP_MID;
        show(frame, br);
        return;
    }
    case ST_LIVE:
    default:
        break;
    }

    /* LIVE: bipolar bar — GREEN (health) grows from the left, RED (hurt) from
     * the right, both shown at once. */
    int max = h->max < 1 ? 1 : h->max;
    int cur = h->cur;
    if (cur < 0) cur = 0;
    if (cur > max) cur = max;
    float f = (float)cur / (float)max;   // health fraction

    /* Allocate the 14 LEDs by health fraction with "fairness": at least 1 LED of
     * each color while both health and damage exist (full HP -> no red; 0 -> no
     * green). */
    int g = (int)lroundf(BAR_LEN * f);
    if (cur > 0   && g < 1)            g = 1;            // alive: >= 1 green
    if (cur < max && g > BAR_LEN - 1)  g = BAR_LEN - 1;  // hurt:  >= 1 red
    if (cur <= 0)   g = 0;
    if (cur >= max) g = BAR_LEN;

    /* Both colors at full intensity — HP is read purely from the green/red
     * split (the amount). Below 50% health the RED pulses, getting both faster
     * and deeper the more hurt you are: ~15 s/cycle and shallow at 50%, ramping
     * to ~6 s/cycle and deep at PULSE_F_LOW (2/32) and below. */
    rgb_t green = GREEN;
    rgb_t red   = RED;
    if (f < 0.5f) {
        float sev    = clampf((0.5f - f) / (0.5f - PULSE_F_LOW), 0.0f, 1.0f);
        float period = PULSE_PERIOD_HI - (PULSE_PERIOD_HI - PULSE_PERIOD_LO) * sev;
        float wave   = 0.5f + 0.5f * sinf(2.0f * (float)M_PI * (t / period)); // 0..1
        float depth  = 0.20f + 0.55f * sev;            // shallow -> deep
        float ps     = (1.0f - depth) + depth * wave;  // (1-depth)..1
        red = (rgb_t){ red.r * ps, red.g * ps, red.b * ps };
    }
    for (int i = 0; i < BAR_LEN; i++)
        frame[BAR_FIRST + i] = (i < g) ? green : red;

    /* detect ANY change in current HP (even sub-LED) -> start a flash. */
    if (*prev_cur >= 0 && cur != *prev_cur) {
        *flash_start_ms = now_ms;
        *flash_is_damage = (cur < *prev_cur);
    }
    *prev_cur = cur;

    float scale = BRIGHTNESS;

    /* hit/heal animation (bar window only): a "splash then drain". The whole bar
     * surges to the flash color (red on damage, green on heal) over FLASH_ATTACK_MS,
     * then the color drains from the top (high index) down to the bottom over the
     * remainder of FLASH_MS before settling back to the steady fill. */
    uint32_t since = now_ms - *flash_start_ms;
    if (*flash_start_ms != 0 && since < FLASH_MS) {
        rgb_t flash = *flash_is_damage ? RED : GREEN;
        if (since < FLASH_ATTACK_MS) {
            float k = (float)since / (float)FLASH_ATTACK_MS;   // 0 -> 1, surge
            for (int i = 0; i < BAR_LEN; i++)
                frame[BAR_FIRST + i] = lerp(frame[BAR_FIRST + i], flash, k);
        } else {
            float p = (float)(since - FLASH_ATTACK_MS) /
                      (float)(FLASH_MS - FLASH_ATTACK_MS);     // 0 -> 1, drain
            float front = (1.0f - p) * (float)BAR_LEN;          // BAR_LEN -> 0
            for (int i = 0; i < BAR_LEN; i++) {
                float k = clampf(front - (float)i, 0.0f, 1.0f); // soft drain edge
                frame[BAR_FIRST + i] = lerp(frame[BAR_FIRST + i], flash, k);
            }
        }
    }

    show(frame, scale);
}

/* ── core1 entry ──────────────────────────────────────────────────────────── */

static void core1_main(void) {
    /* config_save stops us via multicore_reset_core1() before a flash write
     * (always followed by a reboot), so no lockout-victim registration needed. */
    config_set_core1_running(true);

    uint offset = pio_add_program(s_pio, &ws2812_program);
    ws2812_program_init(s_pio, s_sm, offset, LED_GPIO, 800000, false);

    int prev_cur = -1;
    uint32_t flash_start_ms = 0;
    bool flash_is_damage = false;

    while (true) {
        uint32_t now_ms = to_ms_since_boot(get_absolute_time());
        health_t h = health_snapshot();
        render(&h, now_ms, &prev_cur, &flash_start_ms, &flash_is_damage);
        g_last_state = (int)h.state;
        g_last_cur   = h.cur;
        g_frames++;
        /* NOT sleep_ms: on core1, sleep_ms parks on __wfe() waiting for the
         * default alarm pool's IRQ, which is serviced on core0. Once cyw43's
         * lwip_threadsafe_background context is active (after wifi link-up) that
         * wakeup never arrives and core1 hangs here forever. core1 is dedicated
         * to the LED loop, so busy-wait on the raw timer — no alarms, no WFE,
         * no interaction with cyw43. */
        busy_wait_ms(FRAME_MS);
    }
}

void leds_launch(void) {
    /* Dedicated PIO block: cyw43_arch_init() (called before us) claims a state
     * machine on pio0 for the radio's SPI bus. On RP2350 that block also carries
     * a per-block GPIO base tied to the wireless pins, so sharing pio0 with the
     * WS2812 output let the radio stall our SM at wifi link-up — core1 then hung
     * forever in pio_sm_put_blocking. pio1 is untouched by cyw43. */
    s_pio = pio1;
    s_sm = (uint)pio_claim_unused_sm(s_pio, true);
    multicore_launch_core1(core1_main);
}
