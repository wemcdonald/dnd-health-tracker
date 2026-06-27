/**
 * leds.c — WS2812 render engine (M5), runs on core1.
 *
 * Pure time-based animations of the shared health snapshot, so frame rate never
 * affects motion. The device does NO HP math: `lit` (0..NUM_LEDS) arrives
 * precomputed; color comes from the fill fraction lit/NUM_LEDS.
 *
 * Animations (mirroring the MicroPython anim.py):
 *   - gradient green->amber->red by fill fraction
 *   - low-HP heartbeat pulse when lit <= LOW_LIT_THRESHOLD
 *   - damage/heal flash on a change in lit (red on drop, green on rise)
 *   - boot sweep / connecting dot / offline breathing
 *
 * NOTE: not yet verified on hardware (no strip wired at time of writing); it
 * builds clean and the network path is unaffected. core1 never touches cyw43,
 * lwIP, or flash.
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
#define LOW_LIT_THRESHOLD 4         // <= this -> heartbeat (fill below ~0.25)
#define FLASH_MS        220
#define HEARTBEAT_HZ    1.5f

/* theme colors (r,g,b) */
typedef struct { float r, g, b; } rgb_t;
static const rgb_t HP_HIGH = { 0.0f,   0.86f, 0.235f }; // #00dc3c
static const rgb_t HP_MID  = { 0.90f,  0.706f, 0.0f };  // #e6b400
static const rgb_t HP_LOW  = { 0.86f,  0.078f, 0.078f };// #dc1414
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
static volatile int      g_last_lit   = -1;

void leds_diag(uint32_t *frames, int *last_state, int *last_lit) {
    if (frames)     *frames     = g_frames;
    if (last_state) *last_state = g_last_state;
    if (last_lit)   *last_lit   = g_last_lit;
}

void health_init(void) {
    g_lock = spin_lock_init(spin_lock_claim_unused(true));
    g_health.state = ST_BOOT;
    g_health.lit = 0;
    g_health.age_s = 0;
    g_health.updated_ms = 0;
}

void health_set_state(anim_state_t st) {
    uint32_t save = spin_lock_blocking(g_lock);
    g_health.state = st;
    spin_unlock(g_lock, save);
}

void health_set_lit(int lit, int age_s) {
    uint32_t save = spin_lock_blocking(g_lock);
    g_health.state = ST_LIVE;
    g_health.lit = lit;
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

/* Color for a fill fraction f in [0,1]: low -> mid -> high. */
static rgb_t color_for_fraction(float f) {
    if (f >= 0.5f) return lerp(HP_MID, HP_HIGH, (f - 0.5f) / 0.5f);
    return lerp(HP_LOW, HP_MID, (f - 0.0f) / 0.5f);
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

static void show(const rgb_t frame[NUM_LEDS], float scale) {
    for (int i = 0; i < NUM_LEDS; i++) put_pixel(pack_grb(frame[i], scale));
}

/* ── render ───────────────────────────────────────────────────────────────── */

static void render(const health_t *h, uint32_t now_ms, int *prev_lit,
                   uint32_t *flash_start_ms, bool *flash_is_damage) {
    rgb_t frame[NUM_LEDS] = {0};
    float t = now_ms / 1000.0f;

    switch (h->state) {
    case ST_BOOT: {
        int pos = (now_ms / 60) % NUM_LEDS;       // sweeping dot
        frame[pos] = HP_HIGH;
        show(frame, BRIGHTNESS);
        return;
    }
    case ST_CONNECTING: {
        int span = NUM_LEDS - 1;
        int p = (now_ms / 80) % (2 * span);
        int pos = p < span ? p : (2 * span - p);  // bounce
        frame[pos] = HP_MID;
        show(frame, BRIGHTNESS);
        return;
    }
    case ST_OFFLINE: {
        float br = 0.15f + 0.15f * (0.5f + 0.5f * sinf(2.0f * (float)M_PI * 0.25f * t));
        for (int i = 0; i < NUM_LEDS; i++) frame[i] = HP_MID;
        show(frame, br);
        return;
    }
    case ST_LIVE:
    default:
        break;
    }

    /* LIVE: fill `lit` LEDs with the fraction color. */
    int lit = h->lit;
    if (lit < 0) lit = 0;
    if (lit > NUM_LEDS) lit = NUM_LEDS;
    float f = (float)lit / (float)NUM_LEDS;
    rgb_t col = color_for_fraction(f);
    for (int i = 0; i < lit; i++) frame[i] = col;

    /* detect a change in lit -> start a flash. */
    if (*prev_lit >= 0 && lit != *prev_lit) {
        *flash_start_ms = now_ms;
        *flash_is_damage = (lit < *prev_lit);
    }
    *prev_lit = lit;

    float scale = BRIGHTNESS;

    /* low-HP heartbeat. */
    if (lit <= LOW_LIT_THRESHOLD) {
        float pulse = 0.5f + 0.5f * sinf(2.0f * (float)M_PI * HEARTBEAT_HZ * t);
        scale *= (0.35f + 0.65f * pulse);
    }

    /* damage/heal flash overlay (eased fade over FLASH_MS). */
    uint32_t since = now_ms - *flash_start_ms;
    if (*flash_start_ms != 0 && since < FLASH_MS) {
        float k = 1.0f - (float)since / (float)FLASH_MS;  // 1 -> 0
        rgb_t flash = *flash_is_damage ? RED : GREEN;
        for (int i = 0; i < NUM_LEDS; i++) frame[i] = lerp(frame[i], flash, k);
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

    int prev_lit = -1;
    uint32_t flash_start_ms = 0;
    bool flash_is_damage = false;

    while (true) {
        uint32_t now_ms = to_ms_since_boot(get_absolute_time());
        health_t h = health_snapshot();
        render(&h, now_ms, &prev_lit, &flash_start_ms, &flash_is_damage);
        g_last_state = (int)h.state;
        g_last_lit   = h.lit;
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
