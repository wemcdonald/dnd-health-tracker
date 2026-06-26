/**
 * statusd.h — tiny STA-mode status web server + mDNS responder.
 *
 * Serves a read-only status page (and a "reconfigure" button that reboots into
 * the setup portal) on port 80, and advertises healthbar-<slug>.local via mDNS.
 * Runs only in STA mode; lwIP callbacks are serviced by cyw43 threadsafe
 * background on core0 alongside the HTTP poll loop.
 */
#ifndef HEALTHBAR_STATUSD_H
#define HEALTHBAR_STATUSD_H

/* Start the status server + mDNS. `slug` names the page + the mDNS hostname
 * (healthbar-<slug>.local); `poll_desc` is shown on the page (e.g. the URL). */
void statusd_start(const char *slug, const char *poll_desc);

#endif /* HEALTHBAR_STATUSD_H */
