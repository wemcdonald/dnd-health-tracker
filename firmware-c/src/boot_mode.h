/**
 * boot_mode.h — shared boot-mode signaling.
 *
 * PORTAL_FORCE_MAGIC is written to watchdog scratch[0] before a reboot to force
 * the next boot into the setup portal (instead of STA connect). Scratch regs
 * survive a watchdog reboot but reset to 0 on a real power-on. Used by both the
 * STA connect-failure fallback and the status page's "Reconfigure" button.
 */
#ifndef HEALTHBAR_BOOT_MODE_H
#define HEALTHBAR_BOOT_MODE_H

#define PORTAL_FORCE_MAGIC 0x70525430u  /* "pRT0" */

#endif /* HEALTHBAR_BOOT_MODE_H */
