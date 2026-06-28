/**
 * config_choose.c — pure, SDK-free partition-selection logic for config.c.
 *
 * Lives in its own TU so it can be host-compiled into unit tests without
 * pulling in any Pico SDK headers.
 */
#include <stdbool.h>

/*
 * config_choose_source — decide where to read config from.
 *
 * Returns:
 *   0  use the dedicated config partition (normal post-OTA path)
 *   1  use the legacy last-sector and migrate forward into the partition
 *   2  neither location is valid; caller should enter provisioning
 */
int config_choose_source(bool partition_valid, bool legacy_valid) {
    if (partition_valid) return 0;
    if (legacy_valid)    return 1;
    return 2;
}
