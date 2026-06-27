/**
 * Device-facing endpoint: GET /:slug.txt   (e.g. http://dndhealth.willflix.org/shen.txt)
 *
 * Returns the tiny precomputed file the Pico polls. Line 1 is the ONLY line the
 * device parses (`sscanf(body, "%d %d %d %d", &cur, &max, &temp, &age_s)`); the
 * rest is human-readable for browser spot-checking.
 *
 *   <cur> <max> <temp> <age_s>
 *   HP <cur>/<max> (+<temp> temp) · <pct>%
 *
 * - `cur`  current HP (0..max).
 * - `max`  max HP (>= 1 for a live character).
 * - `temp` temporary HP — a separate buffer on top of `cur` (depleted first by
 *          damage, can exceed max, doesn't heal). Carried for the device's
 *          reactive animations; NOT part of the steady fill.
 * - `age_s` seconds since the server last successfully refreshed from D&D Beyond
 *          (upstream staleness). A character that has never refreshed yet returns
 *          the sentinel line `0 0 0 99999` so the device can show a stale/offline
 *          tint.
 */

import type { FastifyInstance } from "fastify";
import { getLiveState, touchCharacter, type LiveState } from "../manager.js";
import { getCharacter } from "../db.js";

const NEVER_AGE_SENTINEL = 99999;

/**
 * Format the full device file body for a character. Pure so it can be unit
 * tested against the wire format the firmware parses. `state` is undefined when
 * the character is registered but has no manager yet; a never-refreshed state
 * (lastUpstreamSuccessMs === 0) gets the sentinel line.
 */
export function formatDeviceFile(state: Readonly<LiveState> | undefined, now: number): string {
  if (!state || state.lastUpstreamSuccessMs === 0) {
    return `0 0 0 ${NEVER_AGE_SENTINEL}\nHP unknown (no upstream data yet)\n`;
  }
  const ageS = Math.floor((now - state.lastUpstreamSuccessMs) / 1000);
  const human = `HP ${state.cur}/${state.max} (+${state.temp} temp) · ${state.pct}%`;
  return `${state.cur} ${state.max} ${state.temp} ${ageS}\n${human}\n`;
}

export async function dndRoutes(app: FastifyInstance): Promise<void> {
  app.get<{ Params: { file: string } }>("/:file", async (req, reply) => {
    const file = req.params.file;
    // Only serve *.txt as character files; everything else at root is not ours.
    if (!file.endsWith(".txt")) {
      return reply.code(404).type("text/plain; charset=utf-8").send("not found\n");
    }
    const slug = file.slice(0, -4);

    // 404 only if the slug isn't even registered; a registered-but-not-yet-
    // refreshed character still returns a parseable body (with sentinel age).
    if (!getCharacter(slug)) {
      return reply.code(404).type("text/plain; charset=utf-8").send(`unknown character: ${slug}\n`);
    }

    // Record the request: keeps this character polling while devices are on, and
    // wakes it (with an immediate refetch) if it had gone idle.
    touchCharacter(slug);

    reply
      .type("text/plain; charset=utf-8")
      .header("Cache-Control", "no-store");

    return reply.send(formatDeviceFile(getLiveState(slug), Date.now()));
  });
}
