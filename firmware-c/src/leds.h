/**
 * leds.h — WS2812 render engine on core1. See leds.c.
 */
#ifndef HEALTHBAR_LEDS_H
#define HEALTHBAR_LEDS_H

#include <stdint.h>

/* Launch the render loop on core1. Call once from core0 after cyw43 init.
 * core1 only touches the LED PIO + the shared health state — never cyw43,
 * lwIP, or flash. */
void leds_launch(void);

/* DIAG (M5 bring-up): snapshot of core1's own render loop, so core0 can show
 * whether core1 is actually alive and what it last rendered (vs. what core0
 * wrote into the shared health state). `frames` advances once per rendered
 * frame; if it's frozen, core1 has stopped. `last_cur` is the current HP core1
 * last rendered. */
void leds_diag(uint32_t *frames, int *last_state, int *last_cur);

#endif /* HEALTHBAR_LEDS_H */
