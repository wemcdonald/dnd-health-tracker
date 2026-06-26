/**
 * provision.h — USB-serial provisioning (no BOOTSEL).
 *
 * Poll for line commands on USB-CDC and update the persisted config:
 *   name <slug>                          set character slug
 *   wifi <ssid> <psk> [<ssid> <psk> ...] set wifi networks (priority = order)
 *   show                                 print current config (no PSKs)
 *   reboot                               reboot
 *
 * Values are percent-encoded by the host (spaces/specials survive the
 * space-delimited line); the firmware URL-decodes each token. `name`/`wifi`
 * save to flash and reboot to apply.
 */
#ifndef HEALTHBAR_PROVISION_H
#define HEALTHBAR_PROVISION_H

/* Drain any pending USB-CDC input and act on completed command lines.
 * Non-blocking; call from the main loops. */
void provision_poll(void);

#endif /* HEALTHBAR_PROVISION_H */
