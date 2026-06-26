/**
 * m0_blink.c — M0 milestone: blink the onboard LED + dual stdio (USB-CDC + UART)
 *
 * Purpose: prove the build chain works before any cyw43/lwIP code is involved.
 * The Pico 2 W's onboard LED is driven via cyw43 GPIO (not a direct RP2350 pin),
 * so we bring in pico_cyw43_arch_none for LED access without the full lwIP stack.
 *
 * Expected serial output (USB or UART @ 115200):
 *   healthbar M0 blink — build OK
 *   LED ON  (count 0)
 *   LED OFF (count 1)
 *   ...
 */

#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"

/* Blink period: 500 ms on, 500 ms off. */
#define BLINK_PERIOD_MS 500

int main(void) {
    stdio_init_all();

    /* Brief delay to let USB CDC enumerate before the first printf. */
    sleep_ms(2000);
    printf("healthbar M0 blink — build OK\n");

    if (cyw43_arch_init()) {
        printf("ERROR: cyw43_arch_init failed\n");
        /* Hang with a fast blink so the problem is visible even without serial. */
        while (true) {
            sleep_ms(100);
        }
    }

    int count = 0;
    while (true) {
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
        printf("LED ON  (count %d)\n", count++);
        sleep_ms(BLINK_PERIOD_MS);

        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
        printf("LED OFF (count %d)\n", count++);
        sleep_ms(BLINK_PERIOD_MS);
    }
}
