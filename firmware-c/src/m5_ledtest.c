/**
 * m5_ledtest.c — STANDALONE WS2812 bring-up test (no cyw43, no wifi, no core1).
 *
 * Purpose: isolate the LED data path from the radio. If this lights all 16 LEDs
 * correctly, then the WS2812 PIO + GP2 + power + strip are all good, and the
 * freeze seen in m1_portal is the cyw43/dual-core interaction (shared pio0).
 *
 * Patterns (each prints a heartbeat over serial so we can confirm core0 alive):
 *   1. walk a single white dot 0..15 (verifies every physical LED + order)
 *   2. solid red, green, blue, white (verifies color order = GRB + all drive)
 *   3. green->amber->red gradient fill (the real health-bar look)
 */
#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "ws2812.pio.h"

#define LED_GPIO   2
#define NUM_LEDS   16

static PIO  s_pio = pio0;
static uint s_sm;

static inline void put_pixel(uint8_t r, uint8_t g, uint8_t b) {
    uint32_t grb = ((uint32_t)g << 16) | ((uint32_t)r << 8) | (uint32_t)b;
    pio_sm_put_blocking(s_pio, s_sm, grb << 8u);
}

static void show(const uint8_t f[NUM_LEDS][3]) {
    for (int i = 0; i < NUM_LEDS; i++) put_pixel(f[i][0], f[i][1], f[i][2]);
}

static void clear(uint8_t f[NUM_LEDS][3]) {
    for (int i = 0; i < NUM_LEDS; i++) f[i][0] = f[i][1] = f[i][2] = 0;
}

int main(void) {
    stdio_init_all();
    sleep_ms(2000);                 // let USB-CDC enumerate
    printf("m5_ledtest: WS2812 on GP%d, %d LEDs, GRB, 800kHz\n", LED_GPIO, NUM_LEDS);

    uint offset = pio_add_program(s_pio, &ws2812_program);
    s_sm = (uint)pio_claim_unused_sm(s_pio, true);
    ws2812_program_init(s_pio, s_sm, offset, LED_GPIO, 800000, false);

    uint8_t f[NUM_LEDS][3];
    uint32_t beat = 0;

    while (true) {
        // 1. single white dot walking the strip
        for (int i = 0; i < NUM_LEDS; i++) {
            clear(f);
            f[i][0] = f[i][1] = f[i][2] = 40;   // dim white at index i
            show(f);
            printf("beat=%lu walk dot=%d\n", (unsigned long)beat++, i);
            sleep_ms(150);
        }

        // 2. solid colors: red, green, blue, white
        const uint8_t solids[4][3] = {
            {60, 0, 0}, {0, 60, 0}, {0, 0, 60}, {40, 40, 40}
        };
        const char *names[4] = {"RED", "GREEN", "BLUE", "WHITE"};
        for (int s = 0; s < 4; s++) {
            for (int i = 0; i < NUM_LEDS; i++) {
                f[i][0] = solids[s][0]; f[i][1] = solids[s][1]; f[i][2] = solids[s][2];
            }
            show(f);
            printf("beat=%lu solid=%s (expect all %d LEDs %s)\n",
                   (unsigned long)beat++, names[s], NUM_LEDS, names[s]);
            sleep_ms(1000);
        }

        // 3. full green->amber->red gradient (all LEDs lit)
        for (int i = 0; i < NUM_LEDS; i++) {
            float t = (float)i / (NUM_LEDS - 1);     // 0=first .. 1=last
            f[i][0] = (uint8_t)(60 * t);             // red rises
            f[i][1] = (uint8_t)(60 * (1.0f - t));    // green falls
            f[i][2] = 0;
        }
        show(f);
        printf("beat=%lu gradient (green->red across the bar)\n", (unsigned long)beat++);
        sleep_ms(1500);
    }
}
