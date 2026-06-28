#ifndef OTA_H
#define OTA_H
#include <stdint.h>
#include <stdbool.h>
#include "lwip/ip_addr.h"

typedef enum { OTA_NONE, OTA_ARMED, OTA_ERROR } ota_result_t;

// Fetch /firmware/latest; if newer than FIRMWARE_VERSION, stream the image into the
// inactive slot, verify whole-image sha256, and arm a TBYB flash-update reboot.
// Returns OTA_ARMED if an update was downloaded+verified+armed (caller should spin
// feeding the watchdog until the ~100ms-delayed reboot fires). OTA_NONE if up to date
// or no firmware/unreachable (non-fatal). OTA_ERROR on a download/verify failure.
ota_result_t ota_check_and_update(const ip_addr_t *srv, uint16_t port, const char *host);

// Commit the running image after a good first poll (idempotent; clears the TBYB flag).
void ota_commit_if_trial(void);

#endif
