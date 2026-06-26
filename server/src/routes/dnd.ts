/**
 * Device-facing endpoint: GET /:slug.txt   (e.g. http://dndhealth.willflix.org/shen.txt)
 *
 * Returns the tiny precomputed file the Pico polls. Line 1 is the ONLY line the
 * device parses (`sscanf(body, "%d %d", &lit, &age_s)`); the rest is human-
 * readable for browser spot-checking.
 *
 *   <lit> <age_s>
 *   HP <cur>/<max> (+<temp> temp) · <pct>%
 *
 * `age_s` is seconds since the server last successfully refreshed from D&D Beyond
 * (upstream staleness). A character that has never refreshed yet returns a large
 * sentinel age so the device can show a stale/offline tint.
 */

import type { FastifyInstance } from "fastify";
import { getLiveState } from "../manager.js";
import { getCharacter } from "../db.js";

const NEVER_AGE_SENTINEL = 99999;

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

    const state = getLiveState(slug);
    reply
      .type("text/plain; charset=utf-8")
      .header("Cache-Control", "no-store");

    if (!state || state.lastUpstreamSuccessMs === 0) {
      return reply.send(`0 ${NEVER_AGE_SENTINEL}\nHP unknown (no upstream data yet)\n`);
    }

    const ageS = Math.floor((Date.now() - state.lastUpstreamSuccessMs) / 1000);
    const human = `HP ${state.cur}/${state.max} (+${state.temp} temp) · ${state.pct}%`;
    return reply.send(`${state.lit} ${ageS}\n${human}\n`);
  });
}
