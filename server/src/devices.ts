/**
 * Pure helpers for board self-registration and config payloads.
 * Wire contract: docs/firmware-contract.md section 4.
 */

import { createHash, timingSafeEqual } from "node:crypto";
import type { Device, DeviceWifi } from "./db.js";

/** Board id: STA MAC as 12 lowercase hex, no separators. */
export const MAC_RE = /^[0-9a-f]{12}$/;
/** Board-generated token: 16 random bytes as 32 lowercase hex. */
export const TOKEN_RE = /^[0-9a-f]{32}$/;
/** Same rule the firmware and the character form use; firmware rejects slugs over 64 chars. */
export const SLUG_RE = /^[a-z0-9._-]{1,64}$/;

export function hashToken(token: string): string {
  return createHash("sha256").update(token).digest("hex");
}

export function tokenMatches(token: string, storedSha256: string): boolean {
  const a = Buffer.from(hashToken(token), "hex");
  const b = Buffer.from(storedSha256, "hex");
  return a.length === b.length && timingSafeEqual(a, b);
}

export function formatMac(mac: string): string {
  return mac.replace(/(..)(?!$)/g, "$1:");
}

export interface ConfigPayload {
  rev: number;
  slug?: string;
  brightness?: number;
  poll_seconds?: number;
  wifi: { upsert: { ssid: string; psk: string; priority: number }[]; remove: string[] };
}

/** The 200 body for GET /device/:mac/config. Unmanaged (null) fields are omitted. */
export function buildConfigPayload(d: Device, wifi: readonly DeviceWifi[]): ConfigPayload {
  return {
    rev: d.configRev,
    ...(d.slug !== null && { slug: d.slug }),
    ...(d.brightness !== null && { brightness: d.brightness }),
    ...(d.pollSeconds !== null && { poll_seconds: d.pollSeconds }),
    wifi: {
      upsert: wifi.filter((w) => w.action === "upsert").map(({ ssid, psk, priority }) => ({ ssid, psk, priority })),
      remove: wifi.filter((w) => w.action === "remove").map((w) => w.ssid),
    },
  };
}
