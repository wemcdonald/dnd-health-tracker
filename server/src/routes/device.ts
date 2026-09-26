/**
 * Board config channel (device-facing, plain HTTP, no Authentik):
 *
 *   GET /device/:mac/config
 *     X-Device-Token: <32 hex>   board-generated; first contact registers it (stored hashed)
 *     X-Config-Rev: <int>        last rev the board applied
 *     X-Firmware-Version, X-Slug, X-Local-IP   reported state, shown in the admin page
 *
 *   -> 400 malformed mac/token, 403 token mismatch, 429 device cap reached
 *      (new registrations only), 304 board is current, 200 JSON ConfigPayload
 *      otherwise (including when X-Config-Rev is missing).
 *
 * Contract: docs/firmware-contract.md section 4.
 */

import type { FastifyInstance, FastifyPluginOptions, FastifyRequest } from "fastify";
import { getDevice, registerDevice, setDeviceToken, recordCheckin, listDeviceWifi, countDevices } from "../db.js";
import { MAC_RE, TOKEN_RE, hashToken, tokenMatches, buildConfigPayload } from "../devices.js";

/**
 * This endpoint is public (no auth proxy in front of it) and a check-in with an
 * unrecognized MAC self-registers a brand-new row. Without a cap, anyone who can
 * reach it could insert unlimited device rows. 64 comfortably covers this
 * deployment's boards; pass `maxDevices` (used by tests) to override.
 */
const MAX_DEVICES = 64;

type Opts = FastifyPluginOptions & { maxDevices?: number };

/** Absent/non-string header -> undefined, so callers can tell "missing" from "". */
function header(req: FastifyRequest, name: string): string | undefined {
  const v = req.headers[name];
  return typeof v === "string" ? v.slice(0, 64) : undefined;
}

export async function deviceRoutes(app: FastifyInstance, opts: Opts = {}): Promise<void> {
  const maxDevices = opts.maxDevices ?? MAX_DEVICES;

  app.get<{ Params: { mac: string } }>("/device/:mac/config", async (req, reply) => {
    const mac = req.params.mac.toLowerCase();
    const token = header(req, "x-device-token");
    if (!MAC_RE.test(mac) || token === undefined || !TOKEN_RE.test(token)) {
      return reply.code(400).type("text/plain").send("bad mac or token\n");
    }

    const now = Date.now();
    const existing = getDevice(mac);
    if (!existing) {
      if (countDevices() >= maxDevices) {
        return reply.code(429).type("text/plain").send("device limit reached\n");
      }
      registerDevice(mac, hashToken(token), now);
    } else if (existing.tokenSha256 === null) {
      setDeviceToken(mac, hashToken(token)); // re-register after "forget"; not a new row
    } else if (!tokenMatches(token, existing.tokenSha256)) {
      return reply.code(403).type("text/plain").send("token mismatch\n");
    }

    recordCheckin(mac, {
      now,
      // Board-reported display fields: absence just means "not reported yet",
      // and "" is already how the db/admin page represent that — safe to
      // coerce at this storage boundary.
      fwVersion: header(req, "x-firmware-version") ?? "",
      reportedSlug: header(req, "x-slug") ?? "",
      localIp: header(req, "x-local-ip") ?? "",
    });

    const dev = getDevice(mac);
    if (!dev) return reply.code(500).type("text/plain").send("device vanished\n");
    const revHeader = header(req, "x-config-rev");
    const clientRev = revHeader === undefined ? NaN : Number(revHeader);
    if (Number.isInteger(clientRev) && clientRev === dev.configRev) {
      return reply.code(304).send();
    }
    return reply
      .header("Cache-Control", "no-store")
      .type("application/json")
      .send(buildConfigPayload(dev, listDeviceWifi(mac)));
  });
}
