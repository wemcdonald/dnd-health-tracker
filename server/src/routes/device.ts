/**
 * Board config channel (device-facing, plain HTTP, no Authentik):
 *
 *   GET /device/:mac/config
 *     X-Device-Token: <32 hex>   board-generated; first contact registers it (stored hashed)
 *     X-Config-Rev: <int>        last rev the board applied
 *     X-Firmware-Version, X-Slug, X-Local-IP   reported state, shown in the admin page
 *
 *   -> 400 malformed mac/token, 403 token mismatch,
 *      304 board is current, 200 JSON ConfigPayload otherwise.
 *
 * Contract: docs/firmware-contract.md section 4.
 */

import type { FastifyInstance, FastifyRequest } from "fastify";
import { getDevice, registerDevice, setDeviceToken, recordCheckin, listDeviceWifi } from "../db.js";
import { MAC_RE, TOKEN_RE, hashToken, tokenMatches, buildConfigPayload } from "../devices.js";

function header(req: FastifyRequest, name: string): string {
  const v = req.headers[name];
  return typeof v === "string" ? v.slice(0, 64) : "";
}

export async function deviceRoutes(app: FastifyInstance): Promise<void> {
  app.get<{ Params: { mac: string } }>("/device/:mac/config", async (req, reply) => {
    const mac = req.params.mac.toLowerCase();
    const token = header(req, "x-device-token");
    if (!MAC_RE.test(mac) || !TOKEN_RE.test(token)) {
      return reply.code(400).type("text/plain").send("bad mac or token\n");
    }

    const now = Date.now();
    const existing = getDevice(mac);
    if (!existing) {
      registerDevice(mac, hashToken(token), now);
    } else if (existing.tokenSha256 === null) {
      setDeviceToken(mac, hashToken(token)); // re-register after "forget"
    } else if (!tokenMatches(token, existing.tokenSha256)) {
      return reply.code(403).type("text/plain").send("token mismatch\n");
    }

    recordCheckin(mac, {
      now,
      fwVersion: header(req, "x-firmware-version"),
      reportedSlug: header(req, "x-slug"),
      localIp: header(req, "x-local-ip"),
    });

    const dev = getDevice(mac);
    if (!dev) return reply.code(500).type("text/plain").send("device vanished\n");
    const clientRev = Number(header(req, "x-config-rev"));
    if (Number.isInteger(clientRev) && clientRev === dev.configRev) {
      return reply.code(304).send();
    }
    return reply
      .header("Cache-Control", "no-store")
      .type("application/json")
      .send(buildConfigPayload(dev, listDeviceWifi(mac)));
  });
}
