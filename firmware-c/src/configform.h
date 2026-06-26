/**
 * configform.h — shared config web form (used by both the AP portal and the
 * STA status page). The two pages are the same form; the status page just adds
 * a live-status table on top.
 */
#ifndef HEALTHBAR_CONFIGFORM_H
#define HEALTHBAR_CONFIGFORM_H

#include <stdbool.h>
#include <stddef.h>

/* Render the full HTTP page (headers + HTML) into out.
 *  - with_status: prepend the live status table (STA mode). false = setup only (AP).
 *  - slug: page title in status mode (ignored otherwise; may be NULL).
 *  - poll_desc: shown in the status table (may be NULL).
 * Pre-fills the form from the current flash config (read on every call). */
int configform_page(char *out, int cap, bool with_status, const char *slug, const char *poll_desc);

/* Render a minimal HTTP page (title + body). */
int configform_simple(char *out, int cap, const char *title, const char *body);

/* Parse the form query string and persist to flash. A blank password keeps the
 * saved one for a network whose SSID matches an existing entry. Returns true on
 * success (>=1 network). */
bool configform_save(const char *qs);

#endif /* HEALTHBAR_CONFIGFORM_H */
