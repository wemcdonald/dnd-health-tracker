/**
 * leds.h — WS2812 render engine on core1. See leds.c.
 */
#ifndef HEALTHBAR_LEDS_H
#define HEALTHBAR_LEDS_H

/* Launch the render loop on core1. Call once from core0 after cyw43 init.
 * core1 only touches the LED PIO + the shared health state — never cyw43,
 * lwIP, or flash. */
void leds_launch(void);

#endif /* HEALTHBAR_LEDS_H */
